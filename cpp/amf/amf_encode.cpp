#include <public/common/AMFFactory.h>
#include <public/common/AMFSTL.h>
#include <public/common/Thread.h>
#include <public/common/TraceAdapter.h>
#include <public/include/components/VideoEncoderAV1.h>
#include <public/include/components/VideoEncoderHEVC.h>
#include <public/include/components/VideoEncoderVCE.h>
#include <public/include/core/Platform.h>
#include <stdio.h>

#include <cstring>
#include <iostream>
#include <math.h>

#include "callback.h"
#include "common.h"
#include "system.h"
#include "util.h"

#define LOG_MODULE "AMFENC"
#include "log.h"

#define AMF_FACILITY L"AMFEncoder"
#define MILLISEC_TIME 10000

namespace {

#define AMF_CHECK_RETURN(res, msg)                                             \
  if (res != AMF_OK) {                                                         \
    LOG_ERROR(std::string(msg) + ", result code: " + std::to_string(int(res)));             \
    return res;                                                                \
  }

amf_int64 amf_usage(int usage) {
  switch (usage) {
  case 1:
    return AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY;
  case 2:
    return AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY;
  case 4:
    return AMF_VIDEO_ENCODER_USAGE_HIGH_QUALITY;
  default:
    return AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY_HIGH_QUALITY;
  }
}

// AMF 只有 4 档画质预设, 把 preset 1..7 就近折进去 (1 最快, 7 画质最好)
amf_int64 amf_quality_preset(int preset, bool hevc) {
  static const amf_int64 avc_presets[] = {
      AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED,
      AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED,
      AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED,
      AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY,
      AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY,
      AMF_VIDEO_ENCODER_QUALITY_PRESET_HIGH_QUALITY,
      AMF_VIDEO_ENCODER_QUALITY_PRESET_HIGH_QUALITY,
  };
  static const amf_int64 hevc_presets[] = {
      AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED,
      AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED,
      AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED,
      AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY,
      AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY,
      AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_HIGH_QUALITY,
      AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_HIGH_QUALITY,
  };
  if (preset >= 1 && preset <= 7) {
    return hevc ? hevc_presets[preset - 1] : avc_presets[preset - 1];
  }
  return hevc ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY
              : AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY;
}

/** Encoder output packet */
struct encoder_packet {
  uint8_t *data; /**< Packet data */
  size_t size;   /**< Packet size */

  int64_t pts; /**< Presentation timestamp */
  int64_t dts; /**< Decode timestamp */

  int32_t timebase_num; /**< Timebase numerator */
  int32_t timebase_den; /**< Timebase denominator */

  bool keyframe; /**< Is a keyframe */

  /* ---------------------------------------------------------------- */
  /* Internal video variables (will be parsed automatically) */

  /* DTS in microseconds */
  int64_t dts_usec;

  /* System DTS in microseconds */
  int64_t sys_dts_usec;
};

class AMFEncoder {

public:
  DataFormat dataFormat_;
  amf::AMFComponentPtr AMFEncoder_ = NULL;
  amf::AMFContextPtr AMFContext_ = NULL;

private:
  // system
  void *handle_;
  // AMF Internals
  AMFFactoryHelper AMFFactory_;
  amf::AMF_MEMORY_TYPE AMFMemoryType_;
  amf::AMF_SURFACE_FORMAT AMFSurfaceFormat_ = amf::AMF_SURFACE_BGRA;
  std::pair<int32_t, int32_t> resolution_;
  amf_wstring codec_;
  // const
  AMF_COLOR_BIT_DEPTH_ENUM eDepth_ = AMF_COLOR_BIT_DEPTH_8;
  int query_timeout_ = ENCODE_TIMEOUT_MS;
  int32_t bitRateIn_;
  int32_t frameRate_;
  int32_t gop_;
  bool enable4K_ = false;
  bool full_range_ = false;
  bool bt709_ = false;
  std::string opts_;

  // Buffers
  std::vector<uint8_t> packetDataBuffer_;

public:
  AMFEncoder(void *handle, amf::AMF_MEMORY_TYPE memoryType, amf_wstring codec,
             DataFormat dataFormat, int32_t width, int32_t height,
             int32_t bitrate, int32_t framerate, int32_t gop,
             const char *opts) {
    handle_ = handle;
    dataFormat_ = dataFormat;
    AMFMemoryType_ = memoryType;
    resolution_ = std::make_pair(width, height);
    codec_ = codec;
    bitRateIn_ = bitrate;
    frameRate_ = framerate;
    gop_ = (gop > 0 && gop < MAX_GOP) ? gop : MAX_GOP;
    enable4K_ = width > 1920 && height > 1080;
    opts_ = opts ? opts : "";
  }

  ~AMFEncoder() {}

  AMF_RESULT encode(void *tex, EncodeCallback callback, void *obj, int64_t ms) {
    amf::AMFSurfacePtr surface = NULL;
    amf::AMFComputeSyncPointPtr pSyncPoint = NULL;
    AMF_RESULT res;
    bool encoded = false;

    switch (AMFMemoryType_) {
    case amf::AMF_MEMORY_DX11:
      // https://github.com/GPUOpen-LibrariesAndSDKs/AMF/issues/280
      // AMF will not copy the surface during the CreateSurfaceFromDX11Native
      // call
      res = AMFContext_->CreateSurfaceFromDX11Native(tex, &surface, NULL);
      AMF_CHECK_RETURN(res, "CreateSurfaceFromDX11Native failed");
      {
        amf::AMFDataPtr data1;
        surface->Duplicate(surface->GetMemoryType(), &data1);
        surface = amf::AMFSurfacePtr(data1);
      }
      break;
    default:
      LOG_ERROR(std::string("Unsupported memory type"));
      return AMF_NOT_IMPLEMENTED;
      break;
    }
    surface->SetPts(ms * AMF_MILLISECOND);
    res = AMFEncoder_->SubmitInput(surface);
    AMF_CHECK_RETURN(res, "SubmitInput failed");

    amf::AMFDataPtr data = NULL;
    res = AMFEncoder_->QueryOutput(&data);
    if (res == AMF_OK && data != NULL) {
      struct encoder_packet packet;
      PacketKeyframe(data, &packet);
      amf::AMFBufferPtr pBuffer = amf::AMFBufferPtr(data);
      packet.size = pBuffer->GetSize();
      if (packet.size > 0) {
        if (packetDataBuffer_.size() < packet.size) {
          size_t newBufferSize = (size_t)exp2(ceil(log2((double)packet.size)));
          packetDataBuffer_.resize(newBufferSize);
        }
        packet.data = packetDataBuffer_.data();
        std::memcpy(packet.data, pBuffer->GetNative(), packet.size);
        if (callback)
          callback(packet.data, packet.size, packet.keyframe, obj, ms);
        encoded = true;
      }
      pBuffer = NULL;
    }
    data = NULL;
    pSyncPoint = NULL;
    surface = NULL;
    return encoded ? AMF_OK : AMF_FAIL;
  }

  AMF_RESULT destroy() {
    if (AMFEncoder_) {
      AMFEncoder_->Terminate();
      AMFEncoder_ = NULL;
    }
    if (AMFContext_) {
      AMFContext_->Terminate();
      AMFContext_ = NULL; // AMFContext_ is the last
    }
    AMFFactory_.Terminate();
    return AMF_OK;
  }

  AMF_RESULT test() {
    AMF_RESULT res = AMF_OK;
    amf::AMFSurfacePtr surface = nullptr;
    res = AMFContext_->AllocSurface(AMFMemoryType_, AMFSurfaceFormat_,
                                    resolution_.first, resolution_.second,
                                    &surface);
    AMF_CHECK_RETURN(res, "AllocSurface failed");
    if (surface->GetPlanesCount() < 1)
      return AMF_FAIL;
    void *native = surface->GetPlaneAt(0)->GetNative();
    if (!native)
      return AMF_FAIL;
    int32_t key_obj = 0;
    auto start = util::now();
    res = encode(native, util_encode::vram_encode_test_callback, &key_obj, 0);
    int64_t elapsed = util::elapsed_ms(start);
    if (res == AMF_OK && key_obj == 1 && elapsed < TEST_TIMEOUT_MS) {
      return AMF_OK;
    }
    return AMF_FAIL;
  }

  AMF_RESULT initialize() {
    AMF_RESULT res;

    res = AMFFactory_.Init();
    if (res != AMF_OK) {
      std::cerr << "AMF init failed, error code = " << res << "\n";
      return res;
    }
    amf::AMFSetCustomTracer(AMFFactory_.GetTrace());
    amf::AMFTraceEnableWriter(AMF_TRACE_WRITER_CONSOLE, true);
    amf::AMFTraceSetWriterLevel(AMF_TRACE_WRITER_CONSOLE, AMF_TRACE_WARNING);

    // AMFContext_
    res = AMFFactory_.GetFactory()->CreateContext(&AMFContext_);
    AMF_CHECK_RETURN(res, "CreateContext failed");

    switch (AMFMemoryType_) {
    case amf::AMF_MEMORY_DX11:
      res = AMFContext_->InitDX11(handle_); // can be DX11 device
      AMF_CHECK_RETURN(res, "InitDX11 failed");
      break;
    default:
      LOG_ERROR(std::string("unsupported amf memory type"));
      return AMF_FAIL;
    }

    // component: encoder
    res = AMFFactory_.GetFactory()->CreateComponent(AMFContext_, codec_.c_str(),
                                                    &AMFEncoder_);
    AMF_CHECK_RETURN(res, "CreateComponent failed");

    res = SetParams(codec_);
    AMF_CHECK_RETURN(res, "Could not set params in encoder.");

    res = AMFEncoder_->Init(AMFSurfaceFormat_, resolution_.first,
                            resolution_.second);
    AMF_CHECK_RETURN(res, "encoder->Init() failed");

    return AMF_OK;
  }

private:
  AMF_RESULT SetParams(const amf_wstring &codecStr) {
    AMF_RESULT res;
    const bool hevc = codecStr == amf_wstring(AMFVideoEncoder_HEVC);
    auto opts = util_encode::parse_opts(opts_.c_str());
    const int usage = util_encode::opt_int(opts, "usage", 0);
    const int preset = util_encode::opt_int(opts, "preset", 0);
    const int rc = util_encode::opt_int(opts, "rc", 0);
    const int q = util_encode::opt_int(opts, "q", -1);
    const int num_ref_frame = util_encode::opt_int(opts, "num_ref_frame", 0);
    if (codecStr == amf_wstring(AMFVideoEncoderVCE_AVC)) {
      // ------------- Encoder params usage---------------
      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_USAGE, amf_usage(usage));
      AMF_CHECK_RETURN(res, "SetProperty AMF_VIDEO_ENCODER_USAGE failed");

      // ------------- Encoder params static---------------
      res = AMFEncoder_->SetProperty(
          AMF_VIDEO_ENCODER_FRAMESIZE,
          ::AMFConstructSize(resolution_.first, resolution_.second));
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_FRAMESIZE failed, (" +
                           std::to_string(resolution_.first) + "," +
                           std::to_string(resolution_.second) + ")");
      res = AMFEncoder_->SetProperty(
          AMF_VIDEO_ENCODER_LOWLATENCY_MODE,
          util_encode::opt_flag(opts, "lowlatency_mode", true));
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_LOWLATENCY_MODE failed");
      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET,
                                     amf_quality_preset(preset, hevc));
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_QUALITY_PRESET failed");
      res =
          AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_COLOR_BIT_DEPTH, eDepth_);
      AMF_CHECK_RETURN(res,
                       "SetProperty(AMF_VIDEO_ENCODER_COLOR_BIT_DEPTH  failed");
      if (rc == 2) {
        res = AMFEncoder_->SetProperty(
            AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
            AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR);
      } else if (rc == 3 && q >= 0 && q <= 51) {
        res = AMFEncoder_->SetProperty(
            AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
            AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP);
      } else {
        res = AMFEncoder_->SetProperty(
            AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
            AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
      }
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD");
      if (rc == 3 && q >= 0 && q <= 51) {
        AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_QP_I, q);
        AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_QP_P, q);
        AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_QP_B, q);
      }
      if (util_encode::has_opt(opts, "vbaq")) {
        AMFEncoder_->SetProperty(
            AMF_VIDEO_ENCODER_ENABLE_VBAQ,
            util_encode::opt_flag(opts, "vbaq", false));
      }
      if (util_encode::has_opt(opts, "enforce_hrd")) {
        AMFEncoder_->SetProperty(
            AMF_VIDEO_ENCODER_ENFORCE_HRD,
            util_encode::opt_flag(opts, "enforce_hrd", false));
      }
      if (util_encode::has_opt(opts, "preanalysis")) {
        AMFEncoder_->SetProperty(
            AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE,
            util_encode::opt_flag(opts, "preanalysis", false));
      }
      if (util_encode::has_opt(opts, "slices_per_frame")) {
        const int slices = util_encode::opt_int(opts, "slices_per_frame", 1);
        if (slices > 0) {
          AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_SLICES_PER_FRAME, slices);
        }
      }
      if (util_encode::has_opt(opts, "high_motion_qb")) {
        AMFEncoder_->SetProperty(
            AMF_VIDEO_ENCODER_HIGH_MOTION_QUALITY_BOOST_ENABLE,
            util_encode::opt_flag(opts, "high_motion_qb", false));
      }
      if (util_encode::has_opt(opts, "input_queue_size")) {
        const int queue = util_encode::opt_int(opts, "input_queue_size", 0);
        if (queue > 0) {
          AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_INPUT_QUEUE_SIZE, queue);
        }
      }
      if (num_ref_frame > 0) {
        AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_MAX_NUM_REFRAMES,
                                 num_ref_frame);
      }
      if (enable4K_) {
        res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_PROFILE,
                                       AMF_VIDEO_ENCODER_PROFILE_HIGH);
        AMF_CHECK_RETURN(res, "SetProperty(AMF_VIDEO_ENCODER_PROFILE failed");

        res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_PROFILE_LEVEL,
                                       AMF_H264_LEVEL__5_1);
        AMF_CHECK_RETURN(res,
                         "SetProperty AMF_VIDEO_ENCODER_PROFILE_LEVEL failed");
      }
      // color
      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_FULL_RANGE_COLOR,
                                     full_range_);
      AMF_CHECK_RETURN(res, "SetProperty AMF_VIDEO_ENCODER_FULL_RANGE_COLOR");
      res = AMFEncoder_->SetProperty<amf_int64>(
          AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE,
          bt709_ ? (full_range_ ? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_709
                                : AMF_VIDEO_CONVERTER_COLOR_PROFILE_709)
                 : (full_range_ ? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_601
                                : AMF_VIDEO_CONVERTER_COLOR_PROFILE_601));
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE");
      // https://github.com/obsproject/obs-studio/blob/e27b013d4754e0e81119ab237ffedce8fcebcbbf/plugins/obs-ffmpeg/texture-amf.cpp#L924
      res = AMFEncoder_->SetProperty<amf_int64>(
          AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC,
          bt709_ ? AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709
                 : AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE170M);
      AMF_CHECK_RETURN(
          res, "SetProperty AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC");
      res = AMFEncoder_->SetProperty<amf_int64>(
          AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES,
          bt709_ ? AMF_COLOR_PRIMARIES_BT709 : AMF_COLOR_PRIMARIES_SMPTE170M);
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES");

      // ------------- Encoder params dynamic ---------------
      AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, 0);
      // do not check error for AMF_VIDEO_ENCODER_B_PIC_PATTERN
      // - can be not supported - check Capability Manager
      // sample
      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_QUERY_TIMEOUT,
                                     query_timeout_); // ms
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_QUERY_TIMEOUT failed");
      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE,
                                     bitRateIn_);
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_TARGET_BITRATE failed");
      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE,
                                     ::AMFConstructRate(frameRate_, 1));
      AMF_CHECK_RETURN(res, "SetProperty AMF_VIDEO_ENCODER_FRAMERATE failed");

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, gop_);
      AMF_CHECK_RETURN(res, "SetProperty AMF_VIDEO_ENCODER_IDR_PERIOD failed");

    } else if (codecStr == amf_wstring(AMFVideoEncoder_HEVC)) {
      // ------------- Encoder params usage---------------
      res =
          AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE,
                                   amf_usage(usage));
      AMF_CHECK_RETURN(res, "SetProperty AMF_VIDEO_ENCODER_HEVC_USAGE failed");

      // ------------- Encoder params static---------------
      res = AMFEncoder_->SetProperty(
          AMF_VIDEO_ENCODER_HEVC_FRAMESIZE,
          ::AMFConstructSize(resolution_.first, resolution_.second));
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_HEVC_FRAMESIZE failed");

      res = AMFEncoder_->SetProperty(
          AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE,
          util_encode::opt_flag(opts, "lowlatency_mode", true));
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_LOWLATENCY_MODE failed");

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
                                     amf_quality_preset(preset, hevc));
      AMF_CHECK_RETURN(
          res, "SetProperty AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET failed");

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH,
                                     eDepth_);
      AMF_CHECK_RETURN(
          res, "SetProperty AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH failed");

      if (rc == 2) {
        res = AMFEncoder_->SetProperty(
            AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
            AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR);
      } else if (rc == 3 && q >= 0 && q <= 51) {
        res = AMFEncoder_->SetProperty(
            AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
            AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CONSTANT_QP);
      } else {
        res = AMFEncoder_->SetProperty(
            AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
            AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR);
      }
      AMF_CHECK_RETURN(
          res, "SetProperty AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD failed");
      if (rc == 3 && q >= 0 && q <= 51) {
        AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_QP_I, q);
        AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_QP_P, q);
      }
      if (util_encode::has_opt(opts, "vbaq")) {
        AMFEncoder_->SetProperty(
            AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ,
            util_encode::opt_flag(opts, "vbaq", false));
      }
      if (util_encode::has_opt(opts, "enforce_hrd")) {
        AMFEncoder_->SetProperty(
            AMF_VIDEO_ENCODER_HEVC_ENFORCE_HRD,
            util_encode::opt_flag(opts, "enforce_hrd", false));
      }
      if (util_encode::has_opt(opts, "preanalysis")) {
        AMFEncoder_->SetProperty(
            AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE,
            util_encode::opt_flag(opts, "preanalysis", false));
      }
      if (util_encode::has_opt(opts, "slices_per_frame")) {
        const int slices = util_encode::opt_int(opts, "slices_per_frame", 1);
        if (slices > 0) {
          AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_SLICES_PER_FRAME,
                                   slices);
        }
      }
      if (util_encode::has_opt(opts, "high_motion_qb")) {
        AMFEncoder_->SetProperty(
            AMF_VIDEO_ENCODER_HEVC_HIGH_MOTION_QUALITY_BOOST_ENABLE,
            util_encode::opt_flag(opts, "high_motion_qb", false));
      }
      if (util_encode::has_opt(opts, "input_queue_size")) {
        const int queue = util_encode::opt_int(opts, "input_queue_size", 0);
        if (queue > 0) {
          AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_INPUT_QUEUE_SIZE,
                                   queue);
        }
      }

      if (enable4K_) {
        res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TIER,
                                       AMF_VIDEO_ENCODER_HEVC_TIER_HIGH);
        AMF_CHECK_RETURN(res, "SetProperty(AMF_VIDEO_ENCODER_HEVC_TIER failed");

        res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_PROFILE_LEVEL,
                                       AMF_LEVEL_5_1);
        AMF_CHECK_RETURN(
            res, "SetProperty AMF_VIDEO_ENCODER_HEVC_PROFILE_LEVEL failed");
      }
      // color
      res = AMFEncoder_->SetProperty<amf_int64>(
          AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE,
          full_range_ ? AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE_FULL
                      : AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE_STUDIO);
      AMF_CHECK_RETURN(
          res, "SetProperty AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE failed");
      res = AMFEncoder_->SetProperty<amf_int64>(
          AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE,
          bt709_ ? (full_range_ ? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_709
                                : AMF_VIDEO_CONVERTER_COLOR_PROFILE_709)
                 : (full_range_ ? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_601
                                : AMF_VIDEO_CONVERTER_COLOR_PROFILE_601));
      AMF_CHECK_RETURN(
          res,
          "SetProperty AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE failed");
      res = AMFEncoder_->SetProperty<amf_int64>(
          AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC,
          bt709_ ? AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709
                 : AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE170M);
      AMF_CHECK_RETURN(
          res, "SetProperty "
               "AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC failed");
      res = AMFEncoder_->SetProperty<amf_int64>(
          AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES,
          bt709_ ? AMF_COLOR_PRIMARIES_BT709 : AMF_COLOR_PRIMARIES_SMPTE170M);
      AMF_CHECK_RETURN(
          res,
          "SetProperty AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES failed");

      // ------------- Encoder params dynamic ---------------
      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT,
                                     query_timeout_); // ms
      AMF_CHECK_RETURN(
          res, "SetProperty(AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT failed");

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE,
                                     bitRateIn_);
      AMF_CHECK_RETURN(
          res, "SetProperty AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE failed");

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE,
                                     ::AMFConstructRate(frameRate_, 1));
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_HEVC_FRAMERATE failed");

      res = AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE,
                                     gop_); // todo
      AMF_CHECK_RETURN(res,
                       "SetProperty AMF_VIDEO_ENCODER_HEVC_GOP_SIZE failed");
    } else {
      return AMF_FAIL;
    }
    LOG_INFO("amf encode params: usage=" + std::to_string(usage) +
             ", preset=" + std::to_string(preset) + ", rc=" +
             std::to_string(rc) + ", q=" + std::to_string(q) + ", kbs=" +
             std::to_string(bitRateIn_ / 1000) + ", gop=" +
             std::to_string(gop_) +
             ", num_ref_frame=" + std::to_string(num_ref_frame));
    return AMF_OK;
  }

  void PacketKeyframe(amf::AMFDataPtr &pData, struct encoder_packet *packet) {
    if (AMFVideoEncoderVCE_AVC == codec_) {
      uint64_t pktType;
      pData->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &pktType);
      packet->keyframe = AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR == pktType ||
                         AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I == pktType;
    } else if (AMFVideoEncoder_HEVC == codec_) {
      uint64_t pktType;
      pData->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &pktType);
      packet->keyframe =
          AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR == pktType ||
          AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_I == pktType;
    }
  }
};

bool convert_codec(DataFormat lhs, amf_wstring &rhs) {
  switch (lhs) {
  case H264:
    rhs = AMFVideoEncoderVCE_AVC;
    break;
  case H265:
    rhs = AMFVideoEncoder_HEVC;
    break;
  default:
    LOG_ERROR(std::string("unsupported codec: ") + std::to_string((int)lhs));
    return false;
  }
  return true;
}

} // namespace
#include "amf_common.cpp"

extern "C" {

int amf_destroy_encoder(void *encoder) {
  try {
    AMFEncoder *enc = (AMFEncoder *)encoder;
    enc->destroy();
    delete enc;
    enc = NULL;
    return 0;
  } catch (const std::exception &e) {
          LOG_ERROR(std::string("destroy failed: ") + e.what());
  }
  return -1;
}

void *amf_new_encoder(void *handle, int64_t luid,
                      DataFormat dataFormat, int32_t width, int32_t height,
                      int32_t kbs, int32_t framerate, int32_t gop,
                      int quality, int rc, int q, int spatial_aq,
                      int temporal_aq, int multipass, int preanalysis,
                      const char *opts) {
  // native AMF path does not consume the ffmpeg-style encode profile args,
  // it reads the vendor options from opts instead
  (void)quality; (void)rc; (void)q; (void)spatial_aq; (void)temporal_aq;
  (void)multipass; (void)preanalysis;
  AMFEncoder *enc = NULL;
  try {
    amf_wstring codecStr;
    if (!convert_codec(dataFormat, codecStr)) {
      return NULL;
    }
    amf::AMF_MEMORY_TYPE memoryType;
    if (!convert_api(memoryType)) {
      return NULL;
    }
    enc = new AMFEncoder(handle, memoryType, codecStr, dataFormat, width,
                         height, kbs * 1000, framerate, gop, opts);
    if (enc) {
      if (AMF_OK == enc->initialize()) {
        return enc;
      }
    }
  } catch (const std::exception &e) {
          LOG_ERROR(std::string("new failed: ") + e.what());
  }
  if (enc) {
    enc->destroy();
    delete enc;
    enc = NULL;
  }
  return NULL;
}

int amf_encode(void *encoder, void *tex, EncodeCallback callback, void *obj,
               int64_t ms) {
  try {
    AMFEncoder *enc = (AMFEncoder *)encoder;
    return -enc->encode(tex, callback, obj, ms);
  } catch (const std::exception &e) {
          LOG_ERROR(std::string("encode failed: ") + e.what());
  }
  return -1;
}

int amf_driver_support() {
  try {
    AMFFactoryHelper factory;
    AMF_RESULT res = factory.Init();
    if (res == AMF_OK) {
      factory.Terminate();
      return 0;
    }
  } catch (const std::exception &e) {
  }
  return -1;
}

int amf_test_encode(int64_t *outLuids, int32_t *outVendors, int32_t maxDescNum, int32_t *outDescNum,
                    DataFormat dataFormat, int32_t width,
                    int32_t height, int32_t kbs, int32_t framerate,
                    int32_t gop, const int64_t *excludedLuids, const int32_t *excludeFormats, int32_t excludeCount) {
  try {
    Adapters adapters;
    if (!adapters.Init(ADAPTER_VENDOR_AMD))
      return -1;
    int count = 0;
    for (auto &adapter : adapters.adapters_) {
      int64_t currentLuid = LUID(adapter.get()->desc1_);
      if (util::skip_test(excludedLuids, excludeFormats, excludeCount, currentLuid, dataFormat)) {
        continue;
      }
      
      AMFEncoder *e = (AMFEncoder *)amf_new_encoder(
          (void *)adapter.get()->device_.Get(), currentLuid,
          dataFormat, width, height, kbs, framerate, gop,
          Quality_Default, RC_CBR, -1, 0, 0, 0, 0, NULL);
      if (!e)
        continue;
      if (e->test() == AMF_OK) {
        outLuids[count] = currentLuid;
        outVendors[count] = VENDOR_AMD;
        count += 1;
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
    LOG_ERROR(std::string("test ") + std::to_string(kbs) + " failed: " + e.what());
  }
  return -1;
}

int amf_set_bitrate(void *encoder, int32_t kbs) {
  try {
    AMFEncoder *enc = (AMFEncoder *)encoder;
    AMF_RESULT res = AMF_FAIL;
    switch (enc->dataFormat_) {
    case H264:
      res = enc->AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE,
                                          kbs * 1000);
      break;
    case H265:
      res = enc->AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE,
                                          kbs * 1000);
      break;
    }
    return res == AMF_OK ? 0 : -1;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("set bitrate to ") + std::to_string(kbs) +
              "k failed: " + e.what());
  }
  return -1;
}

int amf_set_framerate(void *encoder, int32_t framerate) {
  try {
    AMFEncoder *enc = (AMFEncoder *)encoder;
    AMF_RESULT res = AMF_FAIL;
    AMFRate rate = ::AMFConstructRate(framerate, 1);
    switch (enc->dataFormat_) {
    case H264:
      res = enc->AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, rate);
      break;
    case H265:
      res =
          enc->AMFEncoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, rate);
      break;
    }
    return res == AMF_OK ? 0 : -1;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("set framerate to ") + std::to_string(framerate) +
              " failed: " + e.what());
  }
  return -1;
}

} // extern "C"