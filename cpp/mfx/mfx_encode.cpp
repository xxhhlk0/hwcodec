#include <cstring>
#include <deque>
#include <iostream>
#include <libavutil/pixfmt.h>
#include <limits>
#include <sample_defs.h>
#include <sample_utils.h>

#include "callback.h"
#include "common.h"
#include "system.h"
#include "util.h"

#define LOG_MODULE "MFXENC"
#include "log.h"

// #define CONFIG_USE_VPP
#define CONFIG_USE_D3D_CONVERT

#define CHECK_STATUS(X, MSG)                                                   \
  {                                                                            \
    mfxStatus __sts = (X);                                                     \
    if (__sts != MFX_ERR_NONE) {                                               \
      LOG_ERROR(std::string(MSG) + " failed, sts=" + std::to_string((int)__sts));           \
      return __sts;                                                            \
    }                                                                          \
  }

namespace {

// Defined before use: MSVC STL requires std::deque<T> to be instantiated with a
// complete type (libstdc++/libc++ tolerate incomplete types here, MSVC does not).
struct PendingEnc {
  mfxSyncPoint syncp;
  mfxBitstream *bs;
  int64_t ms;
};

mfxStatus MFX_CDECL simple_getHDL(mfxHDL pthis, mfxMemId mid, mfxHDL *handle) {
  mfxHDLPair *pair = (mfxHDLPair *)handle;
  pair->first = mid;
  pair->second = (mfxHDL)(UINT)0;
  return MFX_ERR_NONE;
}

mfxFrameAllocator frameAllocator{{},   NULL,          NULL, NULL,
                                 NULL, simple_getHDL, NULL};

mfxStatus InitSession(MFXVideoSession &session) {
  mfxInitParam mfxparams{};
  mfxIMPL impl = MFX_IMPL_HARDWARE_ANY | MFX_IMPL_VIA_D3D11;
  mfxparams.Implementation = impl;
  mfxparams.Version.Major = 1;
  mfxparams.Version.Minor = 0;
  mfxparams.GPUCopy = MFX_GPUCOPY_OFF;

  return session.InitEx(mfxparams);
}


class VplEncoder {
public:
  std::unique_ptr<NativeDevice> native_ = nullptr;
  MFXVideoSession session_;
  MFXVideoENCODE *mfxENC_ = nullptr;
  std::vector<mfxFrameSurface1> encSurfaces_;
  std::vector<mfxU8> bstData_;
  mfxBitstream mfxBS_;
  mfxVideoParam mfxEncParams_;

  // ---- async encode pipeline ----
  // AsyncDepth=1 时每帧提交后立即 SyncOperation, MFX 编码延迟全额计入采集循环
  // (1440p UHD750 实测空闲 ~8.4ms/帧, DWM 压力下被拉到 16~25ms, fps 卡在 ~44)。
  // AsyncDepth>1 时编码在 GPU 上流水线化: 本帧提交后不等待, 包在后续调用中
  // 收取 (输出滞后约一个采集周期, 换取吞吐)。HWCODEC_ASYNC_DEPTH=1 可退回。
  bool async_ = false;
  std::deque<PendingEnc> pending_;
  std::vector<mfxBitstream> bsRing_;
  std::vector<std::vector<mfxU8>> bsData_;
  // D3D_CONVERT 用的 NV12 转换目标, 与 encSurfaces_ 同索引 (每个在飞帧一份)
  std::vector<ComPtr<ID3D11Texture2D>> nv12Ring_;

  mfxExtBuffer *extbuffers_[4] = {NULL, NULL, NULL, NULL};
  mfxExtCodingOption coding_option_;
  mfxExtCodingOption2 coding_option2_;
  mfxExtCodingOption3 coding_option3_;
  mfxExtVideoSignalInfo signal_info_;
  ComPtr<ID3D11Texture2D> nv12Texture_ = nullptr;

// vpp
#ifdef CONFIG_USE_VPP
  MFXVideoVPP *mfxVPP_ = nullptr;
  mfxVideoParam vppParams_;
  mfxExtBuffer *vppExtBuffers_[1] = {NULL};
  mfxExtVPPDoNotUse vppDontUse_;
  mfxU32 vppDontUseArgList_[4];
  std::vector<mfxFrameSurface1> vppSurfaces_;
#endif

  void *handle_ = nullptr;
  int64_t luid_;
  DataFormat dataFormat_;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int32_t kbs_;
  int32_t framerate_;
  int32_t gop_;
  std::string opts_;

  bool full_range_ = false;
  bool bt709_ = false;

  VplEncoder(void *handle, int64_t luid, DataFormat dataFormat,
             int32_t width, int32_t height, int32_t kbs, int32_t framerate,
             int32_t gop, const char *opts) {
    handle_ = handle;
    luid_ = luid;
    dataFormat_ = dataFormat;
    width_ = width;
    height_ = height;
    kbs_ = kbs;
    framerate_ = framerate;
    gop_ = gop;
    opts_ = opts ? opts : "";
  }

  ~VplEncoder() {}

  mfxStatus Reset() {
    mfxStatus sts = MFX_ERR_NONE;

    if (!native_) {
      native_ = std::make_unique<NativeDevice>();
      if (!native_->Init(luid_, (ID3D11Device *)handle_)) {
        LOG_ERROR(std::string("failed to init native device"));
        return MFX_ERR_DEVICE_FAILED;
      }
    }
    sts = resetMFX();
    CHECK_STATUS(sts, "resetMFX");
#ifdef CONFIG_USE_VPP
    sts = resetVpp();
    CHECK_STATUS(sts, "resetVpp");
#endif
    sts = resetEnc();
    CHECK_STATUS(sts, "resetEnc");
    return MFX_ERR_NONE;
  }

  int encode(ID3D11Texture2D *tex, EncodeCallback callback, void *obj,
             int64_t ms) {
    mfxStatus sts = MFX_ERR_NONE;

    if (async_) {
      // 流水线: 先非阻塞收取已完成包, 在飞深度保持 <= AsyncDepth - 1
      int ret = deliver_ready(callback, obj);
      if (ret < 0)
        return ret;
      // 双保险: 不完全依赖 SDK 对 surface->Data.Locked 的回填, 显式限制在飞
      // 帧数, 否则环形复用的 NV12 目标可能撞上编码器仍在读取的 surface。
      const size_t max_inflight =
          mfxEncParams_.AsyncDepth > 1 ? (size_t)(mfxEncParams_.AsyncDepth - 1)
                                       : (size_t)0;
      auto wait_start = util::now();
      while (pending_.size() > max_inflight) {
        if (util::elapsed_ms(wait_start) > ENCODE_TIMEOUT_MS) {
          LOG_ERROR(std::string("in-flight encode wait timeout"));
          return -1;
        }
        mfxStatus s = session_.SyncOperation(pending_.front().syncp, 10);
        if (MFX_ERR_NONE == s) {
          deliver_one(pending_.front(), callback, obj);
          pending_.pop_front();
        } else if (MFX_WRN_IN_EXECUTION != s) {
          LOG_ERROR(std::string("SyncOperation failed, sts=") +
                    std::to_string((int)s));
          pending_.pop_front();
          return -1;
        }
      }
    }

    int nEncSurfIdx =
        GetFreeSurfaceIndex(encSurfaces_.data(), encSurfaces_.size());
    if (nEncSurfIdx >= (int)encSurfaces_.size()) {
      if (!async_) {
        LOG_ERROR(std::string("no free enc surface"));
        return -1;
      }
      // 回压: 在飞帧占满 surface 时先收已完成包, 没进展就阻塞等最旧的在飞帧
      auto start = util::now();
      while (nEncSurfIdx >= (int)encSurfaces_.size()) {
        if (util::elapsed_ms(start) > ENCODE_TIMEOUT_MS) {
          LOG_ERROR(std::string("no free enc surface"));
          return -1;
        }
        int ret = deliver_ready(callback, obj);
        if (ret < 0)
          return ret;
        if (pending_.empty()) {
          Sleep(1);
        } else {
          // 非阻塞收取没进展 (包还没好): 用短超时阻塞等最旧的一个, 避免空转
          mfxStatus s = session_.SyncOperation(pending_.front().syncp, 10);
          if (MFX_ERR_NONE == s) {
            deliver_one(pending_.front(), callback, obj);
            pending_.pop_front();
          } else if (MFX_WRN_IN_EXECUTION != s) {
            LOG_ERROR(std::string("SyncOperation failed, sts=") +
                      std::to_string((int)s));
            pending_.pop_front();
            return -1;
          }
        }
        nEncSurfIdx =
            GetFreeSurfaceIndex(encSurfaces_.data(), encSurfaces_.size());
      }
    }
    mfxFrameSurface1 *encSurf = &encSurfaces_[nEncSurfIdx];
#ifdef CONFIG_USE_VPP
    mfxSyncPoint syncp;
    sts = vppOneFrame(tex, encSurf, syncp);
    syncp = NULL;
    if (sts != MFX_ERR_NONE) {
      LOG_ERROR(std::string("vppOneFrame failed, sts=") + std::to_string((int)sts));
      return -1;
    }
#elif defined(CONFIG_USE_D3D_CONVERT)
    DXGI_COLOR_SPACE_TYPE colorSpace_in =
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    DXGI_COLOR_SPACE_TYPE colorSpace_out;
    if (bt709_) {
      if (full_range_) {
        colorSpace_out = DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
      } else {
        colorSpace_out = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
      }
    } else {
      if (full_range_) {
        colorSpace_out = DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601;
      } else {
        colorSpace_out = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
      }
    }
    if (!async_) {
      if (!nv12Texture_) {
        D3D11_TEXTURE2D_DESC desc;
        ZeroMemory(&desc, sizeof(desc));
        tex->GetDesc(&desc);
        desc.Format = DXGI_FORMAT_NV12;
        desc.MiscFlags = 0;
        HRI(native_->device_->CreateTexture2D(
            &desc, NULL, nv12Texture_.ReleaseAndGetAddressOf()));
      }
      if (!native_->BgraToNv12(tex, nv12Texture_.Get(), width_, height_,
                               colorSpace_in, colorSpace_out)) {
        LOG_ERROR(std::string("failed to convert to NV12"));
        return -1;
      }
      encSurf->Data.MemId = nv12Texture_.Get();
    } else {
      // NV12 转换目标与 enc surface 一一对应: GetFreeSurfaceIndex 只返回未被
      // SDK 锁定的 surface, 所以同索引的 NV12 纹理必然可以安全覆写。
      // (不能用"上次用到哪"的环形游标: MFX_ERR_MORE_DATA 的帧没有 sync point,
      //  游标可能覆写编码器仍在读取的纹理, 而 surface 的 Locked 标志是权威的。)
      if (nv12Ring_.empty()) {
        D3D11_TEXTURE2D_DESC desc;
        ZeroMemory(&desc, sizeof(desc));
        tex->GetDesc(&desc);
        desc.Format = DXGI_FORMAT_NV12;
        desc.MiscFlags = 0;
        nv12Ring_.resize(encSurfaces_.size());
        for (size_t i = 0; i < nv12Ring_.size(); i++) {
          HRI(native_->device_->CreateTexture2D(
              &desc, NULL, nv12Ring_[i].ReleaseAndGetAddressOf()));
        }
        LOG_INFO("mfx async pipeline: depth=" +
                 std::to_string((int)mfxEncParams_.AsyncDepth) +
                 ", nv12 ring=" + std::to_string(nv12Ring_.size()) +
                 ", enc surfaces=" + std::to_string(encSurfaces_.size()));
      }
      if ((size_t)nEncSurfIdx >= nv12Ring_.size()) {
        LOG_ERROR(std::string("nv12 ring smaller than enc surfaces"));
        return -1;
      }
      ID3D11Texture2D *slotTex = nv12Ring_[nEncSurfIdx].Get();
      if (!native_->BgraToNv12(tex, slotTex, width_, height_, colorSpace_in,
                               colorSpace_out)) {
        LOG_ERROR(std::string("failed to convert to NV12"));
        return -1;
      }
      encSurf->Data.MemId = slotTex;
      return encodeOneFrame(encSurf, nEncSurfIdx, callback, obj, ms);
    }
#else
    encSurf->Data.MemId = tex;
#endif
    return encodeOneFrame(encSurf, -1, callback, obj, ms);
  }

  // 等待所有在飞帧完成并丢弃其输出: 供 Reset/Close 等要求"静止"的操作使用
  // (带在飞帧时调用 MFXVideoENCODE::Reset 是未定义行为)。
  void drain_pending() {
    while (!pending_.empty()) {
      mfxStatus s = session_.SyncOperation(pending_.front().syncp, 1000);
      if (MFX_ERR_NONE != s && MFX_WRN_IN_EXECUTION != s) {
        LOG_ERROR(std::string("SyncOperation failed during drain, sts=") +
                  std::to_string((int)s));
      }
      pending_.pop_front();
    }
  }

  void destroy() {
    if (mfxENC_) {
      // 等待在飞帧完成 (无 callback, 仅同步以释放 surface), 再 Close
      drain_pending();
      //  - It is recommended to close Media SDK components first, before
      //  releasing allocated surfaces, since
      //    some surfaces may still be locked by internal Media SDK resources.
      mfxENC_->Close();
      delete mfxENC_;
      mfxENC_ = NULL;
    }
#ifdef CONFIG_USE_VPP
    if (mfxVPP_) {
      mfxVPP_->Close();
      delete mfxVPP_;
      mfxVPP_ = NULL;
    }
#endif
    // session closed automatically on destruction
  }

private:
  mfxStatus resetMFX() {
    mfxStatus sts = MFX_ERR_NONE;

    sts = InitSession(session_);
    CHECK_STATUS(sts, "InitSession");
    sts = session_.SetHandle(MFX_HANDLE_D3D11_DEVICE, native_->device_.Get());
    CHECK_STATUS(sts, "SetHandle");
    sts = session_.SetFrameAllocator(&frameAllocator);
    CHECK_STATUS(sts, "SetFrameAllocator");

    return MFX_ERR_NONE;
  }

#ifdef CONFIG_USE_VPP
  mfxStatus resetVpp() {
    mfxStatus sts = MFX_ERR_NONE;
    memset(&vppParams_, 0, sizeof(vppParams_));
    vppParams_.IOPattern =
        MFX_IOPATTERN_IN_VIDEO_MEMORY | MFX_IOPATTERN_OUT_VIDEO_MEMORY;
    vppParams_.vpp.In.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    vppParams_.vpp.In.FrameRateExtN = framerate_;
    vppParams_.vpp.In.FrameRateExtD = 1;
    vppParams_.vpp.In.Width = MSDK_ALIGN16(width_);
    vppParams_.vpp.In.Height =
        (MFX_PICSTRUCT_PROGRESSIVE == vppParams_.vpp.In.PicStruct)
            ? MSDK_ALIGN16(height_)
            : MSDK_ALIGN32(height_);
    vppParams_.vpp.In.CropX = 0;
    vppParams_.vpp.In.CropY = 0;
    vppParams_.vpp.In.CropW = width_;
    vppParams_.vpp.In.CropH = height_;
    vppParams_.vpp.In.Shift = 0;
    memcpy(&vppParams_.vpp.Out, &vppParams_.vpp.In, sizeof(vppParams_.vpp.Out));
    vppParams_.vpp.In.FourCC = MFX_FOURCC_RGB4;
    vppParams_.vpp.Out.FourCC = MFX_FOURCC_NV12;
    vppParams_.vpp.In.ChromaFormat = MFX_CHROMAFORMAT_YUV444;
    vppParams_.vpp.Out.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    vppParams_.AsyncDepth = 1;

    vppParams_.ExtParam = vppExtBuffers_;
    vppParams_.NumExtParam = 1;
    vppExtBuffers_[0] = (mfxExtBuffer *)&vppDontUse_;
    vppDontUse_.Header.BufferId = MFX_EXTBUFF_VPP_DONOTUSE;
    vppDontUse_.Header.BufferSz = sizeof(vppDontUse_);
    vppDontUse_.AlgList = vppDontUseArgList_;
    vppDontUse_.NumAlg = 4;
    vppDontUseArgList_[0] = MFX_EXTBUFF_VPP_DENOISE;
    vppDontUseArgList_[1] = MFX_EXTBUFF_VPP_SCENE_ANALYSIS;
    vppDontUseArgList_[2] = MFX_EXTBUFF_VPP_DETAIL;
    vppDontUseArgList_[3] = MFX_EXTBUFF_VPP_PROCAMP;

    if (mfxVPP_) {
      mfxVPP_->Close();
      delete mfxVPP_;
      mfxVPP_ = NULL;
    }
    mfxVPP_ = new MFXVideoVPP(session_);
    if (!mfxVPP_) {
      LOG_ERROR(std::string("Failed to create MFXVideoVPP"));
      return MFX_ERR_MEMORY_ALLOC;
    }

    sts = mfxVPP_->Query(&vppParams_, &vppParams_);
    CHECK_STATUS(sts, "vpp query");
    mfxFrameAllocRequest vppAllocRequest;
    ZeroMemory(&vppAllocRequest, sizeof(vppAllocRequest));
    memcpy(&vppAllocRequest.Info, &vppParams_.vpp.In, sizeof(mfxFrameInfo));
    sts = mfxVPP_->QueryIOSurf(&vppParams_, &vppAllocRequest);
    CHECK_STATUS(sts, "vpp QueryIOSurf");

    vppSurfaces_.resize(vppAllocRequest.NumFrameSuggested);
    for (int i = 0; i < vppAllocRequest.NumFrameSuggested; i++) {
      memset(&vppSurfaces_[i], 0, sizeof(mfxFrameSurface1));
      memcpy(&vppSurfaces_[i].Info, &vppParams_.vpp.In, sizeof(mfxFrameInfo));
    }

    sts = mfxVPP_->Init(&vppParams_);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    CHECK_STATUS(sts, "vpp init");

    return MFX_ERR_NONE;
  }
#endif

  mfxStatus resetEnc() {
    mfxStatus sts = MFX_ERR_NONE;
    memset(&mfxEncParams_, 0, sizeof(mfxEncParams_));

    // Basic
    if (!convert_codec(dataFormat_, mfxEncParams_.mfx.CodecId)) {
      LOG_ERROR(std::string("unsupported dataFormat: ") + std::to_string(dataFormat_));
      return MFX_ERR_UNSUPPORTED;
    }
    // mfxEncParams_.mfx.LowPower = MFX_CODINGOPTION_ON;
    mfxEncParams_.mfx.BRCParamMultiplier = 0;

    // Frame Info
    mfxEncParams_.mfx.FrameInfo.FrameRateExtN = framerate_;
    mfxEncParams_.mfx.FrameInfo.FrameRateExtD = 1;
#ifdef CONFIG_USE_VPP
    mfxEncParams_.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    mfxEncParams_.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
#elif defined(CONFIG_USE_D3D_CONVERT)
    mfxEncParams_.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    mfxEncParams_.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
#else
    mfxEncParams_.mfx.FrameInfo.FourCC = MFX_FOURCC_BGR4;
    mfxEncParams_.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV444;
#endif
    mfxEncParams_.mfx.FrameInfo.BitDepthLuma = 8;
    mfxEncParams_.mfx.FrameInfo.BitDepthChroma = 8;
    mfxEncParams_.mfx.FrameInfo.Shift = 0;
    mfxEncParams_.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    mfxEncParams_.mfx.FrameInfo.CropX = 0;
    mfxEncParams_.mfx.FrameInfo.CropY = 0;
    mfxEncParams_.mfx.FrameInfo.CropW = width_;
    mfxEncParams_.mfx.FrameInfo.CropH = height_;
    // Width must be a multiple of 16
    // Height must be a multiple of 16 in case of frame picture and a multiple
    // of 32 in case of field picture
    mfxEncParams_.mfx.FrameInfo.Width = MSDK_ALIGN16(width_);
    mfxEncParams_.mfx.FrameInfo.Height =
        (MFX_PICSTRUCT_PROGRESSIVE == mfxEncParams_.mfx.FrameInfo.PicStruct)
            ? MSDK_ALIGN16(height_)
            : MSDK_ALIGN32(height_);
    
    // Encoding Options
    mfxEncParams_.mfx.EncodedOrder = 0;

    mfxEncParams_.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;

    auto opts = util_encode::parse_opts(opts_.c_str());

    // Configuration for low latency
    // AsyncDepth=1 时每帧提交后阻塞 SyncOperation; >1 允许 N 帧在飞, 由
    // encode() 延迟收取实现流水线。HWCODEC_ASYNC_DEPTH=1 可退回同步行为。
    mfxEncParams_.AsyncDepth =
        util_encode::opt_int(opts, "async_depth", util_encode::hw_async_depth());
    mfxEncParams_.mfx.GopRefDist =
        1; // 1 is best for low latency, I and P frames only
    mfxEncParams_.mfx.GopPicSize = (gop_ > 0 && gop_ < 0xFFFF) ? gop_ : 0xFFFF;
    // quality
    // https://www.intel.com/content/www/us/en/developer/articles/technical/common-bitrate-control-methods-in-intel-media-sdk.html
    // MFX_TARGETUSAGE_1 是最好画质、MFX_TARGETUSAGE_7 是最快, 与 preset 的
    // "数值越大画质越好" 反向, 故取 8 - preset。
    const int preset = util_encode::opt_int(opts, "preset", 0);
    mfxEncParams_.mfx.TargetUsage = (preset >= 1 && preset <= 7)
                                        ? (mfxU16)(8 - preset)
                                        : MFX_TARGETUSAGE_BEST_SPEED;
    const int q = util_encode::opt_int(opts, "q", -1);
    switch (util_encode::opt_int(opts, "rc", 0)) {
    case 1:
      mfxEncParams_.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
      break;
    case 2:
      mfxEncParams_.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
      break;
    case 3:
      // ICQ 以画质值为目标, 越界时不改码控模式, 由驱动默认 QP 兜底
      if (q > 0 && q <= 51) {
        mfxEncParams_.mfx.RateControlMethod = MFX_RATECONTROL_ICQ;
        mfxEncParams_.mfx.ICQQuality = (mfxU16)q;
      } else {
        mfxEncParams_.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
      }
      break;
    default:
      mfxEncParams_.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
      break;
    }
    mfxEncParams_.mfx.InitialDelayInKB = 0;
    mfxEncParams_.mfx.BufferSizeInKB = 512;
    mfxEncParams_.mfx.TargetKbps = kbs_;
    mfxEncParams_.mfx.MaxKbps = kbs_;
    mfxEncParams_.mfx.NumSlice = 1;
    mfxEncParams_.mfx.NumRefFrame =
        (mfxU16)util_encode::opt_int(opts, "num_ref_frame", 0);
    if (util_encode::has_opt(opts, "low_power")) {
      mfxEncParams_.mfx.LowPower = util_encode::opt_flag(opts, "low_power", false)
                                       ? MFX_CODINGOPTION_ON
                                       : MFX_CODINGOPTION_OFF;
    }

    if (H264 == dataFormat_) {
      mfxEncParams_.mfx.CodecLevel = MFX_LEVEL_AVC_51;
      mfxEncParams_.mfx.CodecProfile = MFX_PROFILE_AVC_MAIN;
    } else if (H265 == dataFormat_) {
      mfxEncParams_.mfx.CodecLevel = MFX_LEVEL_HEVC_51;
      mfxEncParams_.mfx.CodecProfile = MFX_PROFILE_HEVC_MAIN;
    }

    resetEncExtParams();

    // Create Media SDK encoder
    if (mfxENC_) {
      mfxENC_->Close();
      delete mfxENC_;
      mfxENC_ = NULL;
    }
    mfxENC_ = new MFXVideoENCODE(session_);
    if (!mfxENC_) {
      LOG_ERROR(std::string("failed to create MFXVideoENCODE"));
      return MFX_ERR_NOT_INITIALIZED;
    }

    // Validate video encode parameters (optional)
    // - In this example the validation result is written to same structure
    // - MFX_WRN_INCOMPATIBLE_VIDEO_PARAM is returned if some of the video
    // parameters are not supported,
    //   instead the encoder will select suitable parameters closest matching
    //   the requested configuration
    sts = mfxENC_->Query(&mfxEncParams_, &mfxEncParams_);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
    CHECK_STATUS(sts, "Query");
    async_ = mfxEncParams_.AsyncDepth > 1;
    LOG_INFO("mfx encode params: preset=" + std::to_string(preset) +
             ", target_usage=" + std::to_string(mfxEncParams_.mfx.TargetUsage) +
             ", rc=" + std::to_string(mfxEncParams_.mfx.RateControlMethod) +
             ", q=" + std::to_string(q) + ", icq_quality=" +
             std::to_string(mfxEncParams_.mfx.ICQQuality) + ", kbs=" +
             std::to_string(mfxEncParams_.mfx.TargetKbps) + ", gop=" +
             std::to_string(mfxEncParams_.mfx.GopPicSize) +
             ", gop_ref_dist=" + std::to_string(mfxEncParams_.mfx.GopRefDist) +
             ", num_ref_frame=" + std::to_string(mfxEncParams_.mfx.NumRefFrame) +
             ", async_depth=" + std::to_string(mfxEncParams_.AsyncDepth) +
             ", low_power=" + std::to_string(mfxEncParams_.mfx.LowPower));

    mfxFrameAllocRequest EncRequest;
    memset(&EncRequest, 0, sizeof(EncRequest));
    sts = mfxENC_->QueryIOSurf(&mfxEncParams_, &EncRequest);
    CHECK_STATUS(sts, "QueryIOSurf");

    // Allocate surface headers (mfxFrameSurface1) for encoder
    encSurfaces_.resize(EncRequest.NumFrameSuggested);
    for (int i = 0; i < EncRequest.NumFrameSuggested; i++) {
      memset(&encSurfaces_[i], 0, sizeof(mfxFrameSurface1));
      memcpy(&encSurfaces_[i].Info, &mfxEncParams_.mfx.FrameInfo,
             sizeof(mfxFrameInfo));
    }

    // Initialize the Media SDK encoder
    sts = mfxENC_->Init(&mfxEncParams_);
    CHECK_STATUS(sts, "Init");

    // Retrieve video parameters selected by encoder.
    // - BufferSizeInKB parameter is required to set bit stream buffer size
    sts = mfxENC_->GetVideoParam(&mfxEncParams_);
    CHECK_STATUS(sts, "GetVideoParam");

    // Prepare Media SDK bit stream buffer
    if (async_) {
      // 每个在飞帧独立 bitstream (交付前必须保持有效), 按 enc surface 绑定
      size_t n = encSurfaces_.size();
      bsData_.assign(n, {});
      bsRing_.resize(n);
      for (size_t i = 0; i < n; i++) {
        memset(&bsRing_[i], 0, sizeof(mfxBitstream));
        bsRing_[i].MaxLength = mfxEncParams_.mfx.BufferSizeInKB * 1024;
        bsData_[i].resize(bsRing_[i].MaxLength);
        bsRing_[i].Data = bsData_[i].data();
      }
    } else {
      memset(&mfxBS_, 0, sizeof(mfxBS_));
      mfxBS_.MaxLength = mfxEncParams_.mfx.BufferSizeInKB * 1024;
      bstData_.resize(mfxBS_.MaxLength);
      mfxBS_.Data = bstData_.data();
    }

    return MFX_ERR_NONE;
  }

#ifdef CONFIG_USE_VPP
  mfxStatus vppOneFrame(void *texture, mfxFrameSurface1 *out,
                        mfxSyncPoint syncp) {
    mfxStatus sts = MFX_ERR_NONE;

    int surfIdx =
        GetFreeSurfaceIndex(vppSurfaces_.data(),
                            vppSurfaces_.size()); // Find free frame surface
    if (surfIdx >= vppSurfaces_.size()) {
      LOG_ERROR(std::string("No free vpp surface"));
      return MFX_ERR_MORE_SURFACE;
    }
    mfxFrameSurface1 *in = &vppSurfaces_[surfIdx];
    in->Data.MemId = texture;

    for (;;) {
      sts = mfxVPP_->RunFrameVPPAsync(in, out, NULL, &syncp);

      if (MFX_ERR_NONE < sts &&
          !syncp) // repeat the call if warning and no output
      {
        if (MFX_WRN_DEVICE_BUSY == sts)
          MSDK_SLEEP(1); // wait if device is busy
      } else if (MFX_ERR_NONE < sts && syncp) {
        sts = MFX_ERR_NONE; // ignore warnings if output is available
        break;
      } else {
        break; // not a warning
      }
    }

    if (MFX_ERR_NONE == sts) {
      sts = session_.SyncOperation(
          syncp, 1000); // Synchronize. Wait until encoded frame is ready
      CHECK_STATUS(sts, "SyncOperation");
    }

    return sts;
  }
#endif

  void deliver_one(const PendingEnc &p, EncodeCallback callback, void *obj) {
    if (p.bs->DataLength > 0 && callback) {
      int key = (p.bs->FrameType & MFX_FRAMETYPE_I) ||
                (p.bs->FrameType & MFX_FRAMETYPE_IDR);
      callback(p.bs->Data + p.bs->DataOffset, p.bs->DataLength, key, obj,
               p.ms);
    }
  }

  // 非阻塞收取所有已完成的在飞包; 无输出返回 0, 出错返回 -1
  int deliver_ready(EncodeCallback callback, void *obj) {
    while (!pending_.empty()) {
      mfxStatus sts = session_.SyncOperation(pending_.front().syncp, 0);
      if (sts == MFX_WRN_IN_EXECUTION)
        break;
      if (sts != MFX_ERR_NONE) {
        LOG_ERROR(std::string("SyncOperation failed, sts=") +
                  std::to_string((int)sts));
        pending_.pop_front();
        return -1;
      }
      deliver_one(pending_.front(), callback, obj);
      pending_.pop_front();
    }
    return 0;
  }

  int encodeOneFrame(mfxFrameSurface1 *in, int surfIdx,
                     EncodeCallback callback, void *obj, int64_t ms) {
    mfxStatus sts = MFX_ERR_NONE;

    if (async_ && surfIdx >= 0) {
      auto start = util::now();
      for (;;) {
        if (util::elapsed_ms(start) > ENCODE_TIMEOUT_MS) {
          LOG_ERROR(std::string("encode timeout"));
          return -1;
        }
        mfxBitstream &bs = bsRing_[surfIdx];
        bs.DataLength = 0;
        bs.DataOffset = 0;
        bs.TimeStamp = ms * 90; // ms to 90KHZ
        bs.DecodeTimeStamp = bs.TimeStamp;
        mfxSyncPoint syncp = NULL;
        sts = mfxENC_->EncodeFrameAsync(NULL, in, &bs, &syncp);
        if (MFX_ERR_NONE == sts) {
          if (!syncp) {
            LOG_ERROR(std::string(
                "should not happen, error is none while syncp is null"));
            return -1;
          }
          // 提交即返回, 包由后续 encode() 调用收取 (流水线)
          pending_.push_back({syncp, &bs, ms});
          return 0;
        } else if (MFX_ERR_MORE_DATA == sts) {
          // AsyncDepth>1 时编码器要先攒够输入才吐第一个包, 首帧/次帧会返回
          // MFX_ERR_MORE_DATA 且不返回 sync point。这不是错误: 当致命错误处理
          // 会让硬件编码器探测直接失败, 把配置写成 vram_encode=[] 从而禁用
          // VRAM 硬编 (实测: depth=2 时所有 Intel 适配器都被误判为不可用)。
          // 该帧由 SDK 内部持有或丢弃, surface 是否被占用由 Locked 标志决定。
          return 0;
        } else if (MFX_WRN_DEVICE_BUSY == sts) {
          int ret = deliver_ready(callback, obj);
          if (ret < 0)
            return ret;
          if (pending_.empty())
            Sleep(1); // 理论不会发生, 避免空转
          continue;
        } else if (MFX_ERR_NOT_ENOUGH_BUFFER == sts) {
          LOG_ERROR(std::string("not enough buffer, size=") +
                    std::to_string(bs.MaxLength));
          if (bs.MaxLength < 10 * 1024 * 1024) {
            bs.MaxLength *= 2;
            bsData_[surfIdx].resize(bs.MaxLength);
            bs.Data = bsData_[surfIdx].data();
            continue;
          }
          return -1;
        } else {
          LOG_ERROR(std::string("EncodeFrameAsync failed, sts=") +
                    std::to_string((int)sts));
          return -1;
        }
      }
    }

    mfxSyncPoint syncp;
    bool encoded = false;

    auto start = util::now();
    do {
      if (util::elapsed_ms(start) > ENCODE_TIMEOUT_MS) {
        LOG_ERROR(std::string("encode timeout"));
        break;
      }
      mfxBS_.DataLength = 0;
      mfxBS_.DataOffset = 0;
      mfxBS_.TimeStamp = ms * 90; // ms to 90KHZ
      mfxBS_.DecodeTimeStamp = mfxBS_.TimeStamp;
      sts = mfxENC_->EncodeFrameAsync(NULL, in, &mfxBS_, &syncp);
      if (MFX_ERR_NONE == sts) {
        if (!syncp) {
          LOG_ERROR(std::string("should not happen, error is none while syncp is null"));
          break;
        }
        sts = session_.SyncOperation(
            syncp, 1000); // Synchronize. Wait until encoded frame is ready
        if (MFX_ERR_NONE != sts) {
          LOG_ERROR(std::string("SyncOperation failed, sts=") + std::to_string(sts));
          break;
        }
        if (mfxBS_.DataLength <= 0) {
          LOG_ERROR(std::string("mfxBS_.DataLength <= 0"));
          break;
        }
        int key = (mfxBS_.FrameType & MFX_FRAMETYPE_I) ||
                  (mfxBS_.FrameType & MFX_FRAMETYPE_IDR);
        if (callback)
          callback(mfxBS_.Data + mfxBS_.DataOffset, mfxBS_.DataLength, key, obj,
                   ms);
        encoded = true;
        break;
      } else if (MFX_WRN_DEVICE_BUSY == sts) {
        LOG_INFO(std::string("device busy"));
        Sleep(1);
        continue;
      } else if (MFX_ERR_NOT_ENOUGH_BUFFER == sts) {
        LOG_ERROR(std::string("not enough buffer, size=") +
                  std::to_string(mfxBS_.MaxLength));
        if (mfxBS_.MaxLength < 10 * 1024 * 1024) {
          mfxBS_.MaxLength *= 2;
          bstData_.resize(mfxBS_.MaxLength);
          mfxBS_.Data = bstData_.data();
          Sleep(1);
          continue;
        } else {
          break;
        }
      } else {
        LOG_ERROR(std::string("EncodeFrameAsync failed, sts=") + std::to_string(sts));
        break;
      }
      // double confirm, check continue
    } while (MFX_WRN_DEVICE_BUSY == sts || MFX_ERR_NOT_ENOUGH_BUFFER == sts);

    if (!encoded) {
      LOG_ERROR(std::string("encode failed, sts=") + std::to_string(sts));
    }
    return encoded ? 0 : -1;
  }

  void resetEncExtParams() {
    auto opts = util_encode::parse_opts(opts_.c_str());

    // coding option
    memset(&coding_option_, 0, sizeof(mfxExtCodingOption));
    coding_option_.Header.BufferId = MFX_EXTBUFF_CODING_OPTION;
    coding_option_.Header.BufferSz = sizeof(mfxExtCodingOption);
    coding_option_.NalHrdConformance = MFX_CODINGOPTION_OFF;
    if (util_encode::has_opt(opts, "cavlc")) {
      coding_option_.CAVLC = util_encode::opt_flag(opts, "cavlc", false)
                                 ? MFX_CODINGOPTION_ON
                                 : MFX_CODINGOPTION_OFF;
    }
    extbuffers_[0] = (mfxExtBuffer *)&coding_option_;

    // coding option2
    memset(&coding_option2_, 0, sizeof(mfxExtCodingOption2));
    coding_option2_.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
    coding_option2_.Header.BufferSz = sizeof(mfxExtCodingOption2);
    coding_option2_.RepeatPPS = MFX_CODINGOPTION_OFF;
    extbuffers_[1] = (mfxExtBuffer *)&coding_option2_;

    // coding option3
    memset(&coding_option3_, 0, sizeof(mfxExtCodingOption3));
    coding_option3_.Header.BufferId = MFX_EXTBUFF_CODING_OPTION3;
    coding_option3_.Header.BufferSz = sizeof(mfxExtCodingOption3);
    if (util_encode::has_opt(opts, "low_delay_brc")) {
      coding_option3_.LowDelayBRC = util_encode::opt_flag(opts, "low_delay_brc", false)
                                        ? MFX_CODINGOPTION_ON
                                        : MFX_CODINGOPTION_OFF;
    }
    extbuffers_[2] = (mfxExtBuffer *)&coding_option3_;
    
    // signal info
    memset(&signal_info_, 0, sizeof(mfxExtVideoSignalInfo));
    signal_info_.Header.BufferId = MFX_EXTBUFF_VIDEO_SIGNAL_INFO;
    signal_info_.Header.BufferSz = sizeof(mfxExtVideoSignalInfo);
    signal_info_.VideoFormat = 5;
    signal_info_.ColourDescriptionPresent = 1;
    signal_info_.VideoFullRange = !!full_range_;
    signal_info_.MatrixCoefficients =
        bt709_ ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
    signal_info_.ColourPrimaries =
        bt709_ ? AVCOL_PRI_BT709 : AVCOL_PRI_SMPTE170M;
    signal_info_.TransferCharacteristics =
        bt709_ ? AVCOL_TRC_BT709 : AVCOL_TRC_SMPTE170M;
    // https://github.com/GStreamer/gstreamer/blob/651dcb49123ec516e7c582e4a49a5f3f15c10f93/subprojects/gst-plugins-bad/sys/qsv/gstqsvh264enc.cpp#L1647
    extbuffers_[3] = (mfxExtBuffer *)&signal_info_;

    mfxEncParams_.ExtParam = extbuffers_;
    mfxEncParams_.NumExtParam = 4;
  }

  bool convert_codec(DataFormat dataFormat, mfxU32 &CodecId) {
    switch (dataFormat) {
    case H264:
      CodecId = MFX_CODEC_AVC;
      return true;
    case H265:
      CodecId = MFX_CODEC_HEVC;
      return true;
    }
    return false;
  }
};

} // namespace

extern "C" {

int mfx_driver_support() {
  // Never let an exception escape this extern "C" boundary. On some Intel
  // drivers MFXVideoSession::InitEx may throw (including non-std exceptions);
  // letting it propagate out of C calls std::terminate -> abort ->
  // __fastfail(FATAL_APP_EXIT) (0xc0000409 / subcode 7), crashing the host
  // process during startup hardware-codec probing (rustdesk/rustdesk#15218).
  try {
    MFXVideoSession session;
    return InitSession(session) == MFX_ERR_NONE ? 0 : -1;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("mfx_driver_support exception: ") + e.what());
  } catch (...) {
    LOG_ERROR(std::string("mfx_driver_support unknown exception"));
  }
  return -1;
}

int mfx_destroy_encoder(void *encoder) {
  VplEncoder *p = (VplEncoder *)encoder;
  if (p) {
    p->destroy();
    delete p;
    p = NULL;
  }
  return 0;
}

void *mfx_new_encoder(void *handle, int64_t luid,
                      DataFormat dataFormat, int32_t w, int32_t h, int32_t kbs,
                      int32_t framerate, int32_t gop,
                      int quality, int rc, int q, int spatial_aq,
                      int temporal_aq, int multipass, int preanalysis,
                      const char *opts) {
  // native MFX path does not consume the ffmpeg-style encode profile args,
  // it reads the vendor options from opts instead
  (void)quality; (void)rc; (void)q; (void)spatial_aq; (void)temporal_aq;
  (void)multipass; (void)preanalysis;
  VplEncoder *p = NULL;
  try {
    p = new VplEncoder(handle, luid, dataFormat, w, h, kbs, framerate,
                       gop, opts);
    if (!p) {
      return NULL;
    }
    mfxStatus sts = p->Reset();
    if (sts == MFX_ERR_NONE) {
      return p;
    } else {
      LOG_ERROR(std::string("Init failed, sts=") + std::to_string(sts));
    }
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("Exception: ") + e.what());
  } catch (...) {
    LOG_ERROR(std::string("Unknown exception"));
  }

  if (p) {
    p->destroy();
    delete p;
    p = NULL;
  }
  return NULL;
}

int mfx_encode(void *encoder, ID3D11Texture2D *tex, EncodeCallback callback,
               void *obj, int64_t ms) {
  try {
    return ((VplEncoder *)encoder)->encode(tex, callback, obj, ms);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("Exception: ") + e.what());
  } catch (...) {
    LOG_ERROR(std::string("Unknown exception"));
  }
  return -1;
}

int mfx_test_encode(int64_t *outLuids, int32_t *outVendors, int32_t maxDescNum, int32_t *outDescNum,
                    DataFormat dataFormat, int32_t width,
                    int32_t height, int32_t kbs, int32_t framerate,
                    int32_t gop, const int64_t *excludedLuids, const int32_t *excludeFormats, int32_t excludeCount) {
  try {
    Adapters adapters;
    if (!adapters.Init(ADAPTER_VENDOR_INTEL))
      return -1;
    int count = 0;
    for (auto &adapter : adapters.adapters_) {
      int64_t currentLuid = LUID(adapter.get()->desc1_);
      if (util::skip_test(excludedLuids, excludeFormats, excludeCount, currentLuid, dataFormat)) {
        continue;
      }
      
      VplEncoder *e = (VplEncoder *)mfx_new_encoder(
          (void *)adapter.get()->device_.Get(), currentLuid,
          dataFormat, width, height, kbs, framerate, gop,
          Quality_Default, RC_CBR, -1, 0, 0, 0, 0, NULL);
      if (!e)
        continue;
      if (e->native_->EnsureTexture(e->width_, e->height_)) {
        e->native_->next();
        int32_t key_obj = 0;
        auto start = util::now();
        bool succ = false;
        // AsyncDepth>1 时首个包要等编码器攒够输入才产出 (最初几帧返回
        // MFX_ERR_MORE_DATA 且无 sync point)。只提交一帧会把可用的 Intel
        // 编码器误判为不可用, 把配置写成 vram_encode=[], 等于禁用 VRAM 硬编。
        for (int attempt = 0; attempt < 8; attempt++) {
          if (util::elapsed_ms(start) >= TEST_TIMEOUT_MS)
            break;
          if (mfx_encode(e, e->native_->GetCurrentTexture(),
                         util_encode::vram_encode_test_callback, &key_obj,
                         0) != 0)
            break;
          if (key_obj == 1) {
            succ = true;
            break;
          }
        }
        int64_t elapsed = util::elapsed_ms(start);
        if (succ && elapsed < TEST_TIMEOUT_MS) {
          outLuids[count] = currentLuid;
          outVendors[count] = VENDOR_INTEL;
          count += 1;
        }
      }
      e->destroy();
      delete e;
      e = nullptr;
      if (count >= maxDescNum)
        break;
    }
    *outDescNum = count;
    return 0;

  } catch (const std::exception &e) {
    LOG_ERROR(std::string("test failed: ") + e.what());
  } catch (...) {
    LOG_ERROR(std::string("test failed: unknown exception"));
  }
  return -1;
}

// https://github.com/Intel-Media-SDK/MediaSDK/blob/master/doc/mediasdk-man.md#dynamic-bitrate-change
// https://github.com/Intel-Media-SDK/MediaSDK/blob/master/doc/mediasdk-man.md#mfxinfomfx
// https://spec.oneapi.io/onevpl/2.4.0/programming_guide/VPL_prg_encoding.html#configuration-change
int mfx_set_bitrate(void *encoder, int32_t kbs) {
  try {
    VplEncoder *p = (VplEncoder *)encoder;
    mfxStatus sts = MFX_ERR_NONE;
    // https://github.com/GStreamer/gstreamer/blob/e19428a802c2f4ee9773818aeb0833f93509a1c0/subprojects/gst-plugins-bad/sys/qsv/gstqsvencoder.cpp#L1312
    p->kbs_ = kbs;
    // 异步流水线: Reset 前必须等所有在飞帧完成 (带在飞帧 Reset 是未定义行为)
    p->drain_pending();
    p->mfxENC_->GetVideoParam(&p->mfxEncParams_);
    p->mfxEncParams_.mfx.TargetKbps = kbs;
    p->mfxEncParams_.mfx.MaxKbps = kbs;
    sts = p->mfxENC_->Reset(&p->mfxEncParams_);
    if (sts != MFX_ERR_NONE) {
      LOG_ERROR(std::string("reset failed, sts=") + std::to_string(sts));
      return -1;
    }
    return 0;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("Exception: ") + e.what());
  } catch (...) {
    LOG_ERROR(std::string("Unknown exception"));
  }
  return -1;
}

int mfx_set_framerate(void *encoder, int32_t framerate) {
  LOG_WARN("not support change framerate");
  return -1;
}
}
