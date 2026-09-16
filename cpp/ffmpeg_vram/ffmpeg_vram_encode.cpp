extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
}

#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif

#include <memory>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "callback.h"
#include "common.h"
#include "system.h"

#define LOG_MODULE "FFMPEG_VRAM_ENC"
#include <log.h>
#include <util.h>

namespace {

void lockContext(void *lock_ctx);
void unlockContext(void *lock_ctx);

enum class EncoderDriver {
  NVENC,
  AMF,
  QSV,
};

class Encoder {
public:
  Encoder(EncoderDriver driver, const char *name, AVHWDeviceType device_type,
          AVHWDeviceType derived_device_type, AVPixelFormat hw_pixfmt,
          AVPixelFormat sw_pixfmt) {
    driver_ = driver;
    name_ = name;
    device_type_ = device_type;
    derived_device_type_ = derived_device_type;
    hw_pixfmt_ = hw_pixfmt;
    sw_pixfmt_ = sw_pixfmt;
  };
  EncoderDriver driver_;
  std::string name_;
  AVHWDeviceType device_type_;
  AVHWDeviceType derived_device_type_;
  AVPixelFormat hw_pixfmt_;
  AVPixelFormat sw_pixfmt_;
};

class FFmpegVRamEncoder {
public:
  AVCodecContext *c_ = NULL;
  AVBufferRef *hw_device_ctx_ = NULL;
  AVFrame *frame_ = NULL;
  AVFrame *mapped_frame_ = NULL;
  ID3D11Texture2D *encode_texture_ = NULL; // no free
  AVPacket *pkt_ = NULL;
  std::unique_ptr<NativeDevice> native_ = nullptr;
  ID3D11Device *d3d11Device_ = NULL;
  ID3D11DeviceContext *d3d11DeviceContext_ = NULL;
  std::unique_ptr<Encoder> encoder_ = nullptr;

  void *handle_ = nullptr;
  int64_t luid_;
  DataFormat dataFormat_;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int32_t kbs_;
  int32_t framerate_;
  int32_t gop_;
  int quality_;
  int rc_;
  int q_;
  int spatial_aq_;
  int temporal_aq_;
  int multipass_;
  int preanalysis_;
  bool enhance_applied_ = false;
  // 是否下发过 qsv 的 low_power/low_delay_brc (avcodec_open2 失败时据此回退重试)
  bool qsv_low_latency_applied_ = false;

  const int align_ = 0;
  const bool full_range_ = false;
  const bool bt709_ = false;
  FFmpegVRamEncoder(void *handle, int64_t luid, DataFormat dataFormat,
                    int32_t width, int32_t height, int32_t kbs,
                    int32_t framerate, int32_t gop, int quality, int rc,
                    int q, int spatial_aq, int temporal_aq, int multipass,
                    int preanalysis) {
    handle_ = handle;
    luid_ = luid;
    dataFormat_ = dataFormat;
    width_ = width;
    height_ = height;
    kbs_ = kbs;
    framerate_ = framerate;
    gop_ = gop;
    quality_ = quality;
    rc_ = rc;
    q_ = q;
    spatial_aq_ = spatial_aq;
    temporal_aq_ = temporal_aq;
    multipass_ = multipass;
    preanalysis_ = preanalysis;
  }

  ~FFmpegVRamEncoder() {}

  bool init() {
    const AVCodec *codec = NULL;
    int ret;

    native_ = std::make_unique<NativeDevice>();
    if (!native_->Init(luid_, (ID3D11Device *)handle_)) {
      LOG_ERROR(std::string("NativeDevice init failed"));
      return false;
    }
    d3d11Device_ = native_->device_.Get();
    d3d11Device_->AddRef();
    d3d11DeviceContext_ = native_->context_.Get();
    d3d11DeviceContext_->AddRef();

    AdapterVendor vendor = native_->GetVendor();
    if (!choose_encoder(vendor)) {
      return false;
    }
          LOG_INFO(std::string("encoder name: ") + encoder_->name_);
    if (!(codec = avcodec_find_encoder_by_name(encoder_->name_.c_str()))) {
      LOG_ERROR(std::string("Codec ") + encoder_->name_ + " not found");
      return false;
    }

    if (!(c_ = avcodec_alloc_context3(codec))) {
      LOG_ERROR(std::string("Could not allocate video codec context"));
      return false;
    }

    /* resolution must be a multiple of two */
    c_->width = width_;
    c_->height = height_;
    c_->pix_fmt = encoder_->hw_pixfmt_;
    c_->sw_pix_fmt = encoder_->sw_pixfmt_;
    util_encode::set_av_codec_ctx(c_, encoder_->name_, kbs_, gop_, framerate_);
    if (!util_encode::set_lantency_free(c_->priv_data, encoder_->name_)) {
      return false;
    }
    // qsv: low_power + low_delay_brc (吞吐相关; 老核显不支持时由下方 open 回退)
    qsv_low_latency_applied_ =
        util_encode::apply_qsv_low_latency(c_->priv_data, encoder_->name_);
    // preset/quality: previously commented out (Quality_Default is a no-op), so the
    // encode profile preset had no effect on the vram path. Same mapping as the RAM path.
    if (!util_encode::set_quality(c_->priv_data, encoder_->name_, quality_)) {
      // do not fail the session, just keep the encoder default preset
      LOG_ERROR(std::string("set_quality failed, keep the default preset, name: ") +
                encoder_->name_);
    }
    // rc: RC_DEFAULT (no profile) keeps the legacy behaviour of this path (CBR)
    util_encode::set_rate_control(c_, encoder_->name_,
                                  rc_ == RC_DEFAULT ? RC_CBR : (RateControl)rc_,
                                  q_);
    if (spatial_aq_ > 0 || temporal_aq_ > 0 || multipass_ > 0 ||
        preanalysis_ > 0) {
      enhance_applied_ = util_encode::set_encode_enhance(
          c_->priv_data, encoder_->name_, spatial_aq_, temporal_aq_,
          multipass_, preanalysis_);
    }
    util_encode::set_others(c_->priv_data, encoder_->name_);
    // 与 RAM 通道一致: 打印请求参数 + open 后 ffmpeg 实际选定的码控字段,
    // 便于确认 rc/preset/QP 是否真的生效 (qsv + rc=CQ -> ICQ, global_quality = q)
    LOG_INFO("hw encode params: name=" + encoder_->name_ +
             ", quality=" + std::to_string(quality_) + ", rc=" +
             std::to_string(rc_) + ", q=" + std::to_string(q_) +
             ", kbs=" + std::to_string(kbs_) + ", fps=" +
             std::to_string(framerate_) + ", gop=" + std::to_string(gop_) +
             ", bit_rate=" + std::to_string(c_->bit_rate) +
             ", rc_max_rate=" + std::to_string(c_->rc_max_rate) +
             ", global_quality=" + std::to_string(c_->global_quality));

    hw_device_ctx_ = av_hwdevice_ctx_alloc(encoder_->device_type_);
    if (!hw_device_ctx_) {
      LOG_ERROR(std::string("av_hwdevice_ctx_create failed"));
      return false;
    }

    AVHWDeviceContext *deviceContext =
        (AVHWDeviceContext *)hw_device_ctx_->data;
    AVD3D11VADeviceContext *d3d11vaDeviceContext =
        (AVD3D11VADeviceContext *)deviceContext->hwctx;
    d3d11vaDeviceContext->device = d3d11Device_;
    d3d11vaDeviceContext->device_context = d3d11DeviceContext_;
    d3d11vaDeviceContext->lock = lockContext;
    d3d11vaDeviceContext->unlock = unlockContext;
    d3d11vaDeviceContext->lock_ctx = this;
    ret = av_hwdevice_ctx_init(hw_device_ctx_);
    if (ret < 0) {
      LOG_ERROR(std::string("av_hwdevice_ctx_init failed, ret = ") + av_err2str(ret));
      return false;
    }
    if (encoder_->derived_device_type_ != AV_HWDEVICE_TYPE_NONE) {
      AVBufferRef *derived_context = nullptr;
      ret = av_hwdevice_ctx_create_derived(
          &derived_context, encoder_->derived_device_type_, hw_device_ctx_, 0);
      if (ret) {
            LOG_ERROR(std::string("av_hwdevice_ctx_create_derived failed, err = ") +
              av_err2str(ret));
        return false;
      }
      av_buffer_unref(&hw_device_ctx_);
      hw_device_ctx_ = derived_context;
    }
    c_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    if (!set_hwframe_ctx()) {
      return false;
    }

    if (!(pkt_ = av_packet_alloc())) {
      LOG_ERROR(std::string("Could not allocate video packet"));
      return false;
    }

    int open_ret = avcodec_open2(c_, codec, NULL);
    if (open_ret < 0 && enhance_applied_) {
      // 画质增强是可选项: 部分 GPU/驱动会拒绝。此时去掉增强项重试一次,
      // 而不是让远程会话建不起来 (与 RAM 通道行为一致)。
      LOG_WARN(std::string("avcodec_open2 failed with encode enhancement, retry "
                           "without them, ret = ") +
               av_err2str(open_ret) + ", name: " + encoder_->name_);
      util_encode::set_encode_enhance(c_->priv_data, encoder_->name_, 0, 0, 0,
                                      0);
      enhance_applied_ = false;
      open_ret = avcodec_open2(c_, codec, NULL);
    }
    if (open_ret < 0 && qsv_low_latency_applied_) {
      // low_power (VDENC) 在部分老核显/驱动上不被支持, 会让 open 直接失败。
      // 回退成驱动默认再试一次, 而不是让远程会话建不起来。
      LOG_WARN(std::string("avcodec_open2 failed with qsv low_power/low_delay_brc, "
                           "retry without them, ret = ") +
               av_err2str(open_ret) + ", name: " + encoder_->name_);
      util_encode::revert_qsv_low_latency(c_->priv_data, encoder_->name_);
      qsv_low_latency_applied_ = false;
      open_ret = avcodec_open2(c_, codec, NULL);
    }
    if (open_ret < 0) {
      LOG_ERROR(std::string("avcodec_open2 failed, ret = ") +
                av_err2str(open_ret) + ", name: " + encoder_->name_);
      return false;
    }
    // open 之后 ffmpeg 才真正选定码控模式, 并可能回填这些字段;
    // 排查"参数到底生效没有"以这一行为准 (与上面 hw encode params 对比)。
    LOG_INFO("hw encode opened: name=" + encoder_->name_ +
             ", bit_rate=" + std::to_string(c_->bit_rate) +
             ", rc_max_rate=" + std::to_string(c_->rc_max_rate) +
             ", global_quality=" + std::to_string(c_->global_quality));

    if (!(frame_ = av_frame_alloc())) {
      LOG_ERROR(std::string("Could not allocate video frame"));
      return false;
    }
    frame_->format = c_->pix_fmt;
    frame_->width = c_->width;
    frame_->height = c_->height;
    frame_->color_range = c_->color_range;
    frame_->color_primaries = c_->color_primaries;
    frame_->color_trc = c_->color_trc;
    frame_->colorspace = c_->colorspace;
    frame_->chroma_location = c_->chroma_sample_location;

    if ((ret = av_hwframe_get_buffer(c_->hw_frames_ctx, frame_, 0)) < 0) {
      LOG_ERROR(std::string("av_frame_get_buffer failed, ret = ") + av_err2str(ret));
      return false;
    }
    if (frame_->format == AV_PIX_FMT_QSV) {
      mapped_frame_ = av_frame_alloc();
      if (!mapped_frame_) {
        LOG_ERROR(std::string("Could not allocate mapped video frame"));
        return false;
      }
      mapped_frame_->format = AV_PIX_FMT_D3D11;
      ret = av_hwframe_map(mapped_frame_, frame_,
                           AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE);
      if (ret) {
        LOG_ERROR(std::string("av_hwframe_map failed, err = ") + av_err2str(ret));
        return false;
      }
      encode_texture_ = (ID3D11Texture2D *)mapped_frame_->data[0];
    } else {
      encode_texture_ = (ID3D11Texture2D *)frame_->data[0];
    }

    return true;
  }

  int encode(void *texture, EncodeCallback callback, void *obj, int64_t ms) {

    if (!convert(texture))
      return -1;

    return do_encode(callback, obj, ms);
  }

  void destroy() {
    if (pkt_)
      av_packet_free(&pkt_);
    if (frame_)
      av_frame_free(&frame_);
    if (mapped_frame_)
      av_frame_free(&mapped_frame_);
    if (c_)
      avcodec_free_context(&c_);
    if (hw_device_ctx_) {
      av_buffer_unref(&hw_device_ctx_);
      // AVHWDeviceContext takes ownership of d3d11 object
      d3d11Device_ = nullptr;
      d3d11DeviceContext_ = nullptr;
    } else {
      SAFE_RELEASE(d3d11Device_);
      SAFE_RELEASE(d3d11DeviceContext_);
    }
  }

  int set_bitrate(int kbs) {
    return util_encode::change_bit_rate(c_, encoder_->name_, kbs) ? 0 : -1;
  }

  int set_framerate(int framerate) {
    c_->time_base = av_make_q(1, framerate);
    c_->framerate = av_inv_q(c_->time_base);
    return 0;
  }

private:
  bool choose_encoder(AdapterVendor vendor) {
    if (ADAPTER_VENDOR_NVIDIA == vendor) {
      const char *name = nullptr;
      if (dataFormat_ == H264) {
        name = "h264_nvenc";
      } else if (dataFormat_ == H265) {
        name = "hevc_nvenc";
      } else {
        LOG_ERROR(std::string("Unsupported data format: ") + std::to_string(dataFormat_));
        return false;
      }
      encoder_ = std::make_unique<Encoder>(
          EncoderDriver::NVENC, name, AV_HWDEVICE_TYPE_D3D11VA,
          AV_HWDEVICE_TYPE_NONE, AV_PIX_FMT_D3D11, AV_PIX_FMT_NV12);
      return true;
    } else if (ADAPTER_VENDOR_AMD == vendor) {
      const char *name = nullptr;
      if (dataFormat_ == H264) {
        name = "h264_amf";
      } else if (dataFormat_ == H265) {
        name = "hevc_amf";
      } else {
        LOG_ERROR(std::string("Unsupported data format: ") + std::to_string(dataFormat_));
        return false;
      }
      encoder_ = std::make_unique<Encoder>(
          EncoderDriver::AMF, name, AV_HWDEVICE_TYPE_D3D11VA,
          AV_HWDEVICE_TYPE_NONE, AV_PIX_FMT_D3D11, AV_PIX_FMT_NV12);
      return true;
    } else if (ADAPTER_VENDOR_INTEL == vendor) {
      const char *name = nullptr;
      if (dataFormat_ == H264) {
        name = "h264_qsv";
      } else if (dataFormat_ == H265) {
        name = "hevc_qsv";
      } else {
        LOG_ERROR(std::string("Unsupported data format: ") + std::to_string(dataFormat_));
        return false;
      }
      encoder_ = std::make_unique<Encoder>(
          EncoderDriver::QSV, name, AV_HWDEVICE_TYPE_D3D11VA,
          AV_HWDEVICE_TYPE_QSV, AV_PIX_FMT_QSV, AV_PIX_FMT_NV12);
      return true;
    } else {
      LOG_ERROR(std::string("Unsupported vendor: ") + std::to_string(vendor));
      return false;
    }
    return false;
  }
  int do_encode(EncodeCallback callback, const void *obj, int64_t ms) {
    int ret;
    bool encoded = false;
    frame_->pts = ms;
    if ((ret = avcodec_send_frame(c_, frame_)) < 0) {
      LOG_ERROR(std::string("avcodec_send_frame failed, ret = ") + av_err2str(ret));
      return ret;
    }

    auto start = util::now();
    while (ret >= 0 && util::elapsed_ms(start) < ENCODE_TIMEOUT_MS) {
      ret = avcodec_receive_packet(c_, pkt_);
      if (ret == AVERROR(EAGAIN)) {
        ret = 0;
        if (encoded) {
          // 当前帧的包已交付且管线已空: 与上游语义一致, 正常收工,
          // 不能在这里空等到 ENCODE_TIMEOUT_MS, 否则每帧都会白等 1 秒。
          break;
        }
        // async_depth > 1 时首帧还没吐包: 需要等一会儿再取, 直接返回失败会被
        // 调用方当成编码错误 (首帧甚至会切掉硬件编码器)。
        util::sleep_ms(1);
        continue;
      }
      if (ret < 0) {
        LOG_ERROR(std::string("avcodec_receive_packet failed, ret = ") + av_err2str(ret));
        goto _exit;
      }
      if (!pkt_->data || !pkt_->size) {
        LOG_ERROR(std::string("avcodec_receive_packet failed, pkt size is 0"));
        goto _exit;
      }
      encoded = true;
      if (callback)
        callback(pkt_->data, pkt_->size, pkt_->flags & AV_PKT_FLAG_KEY, obj,
                 pkt_->pts);
    }
  _exit:
    av_packet_unref(pkt_);
    return encoded ? 0 : -1;
  }

  bool convert(void *texture) {
    if (frame_->format == AV_PIX_FMT_D3D11 ||
        frame_->format == AV_PIX_FMT_QSV) {
      ID3D11Texture2D *texture2D = (ID3D11Texture2D *)encode_texture_;
      D3D11_TEXTURE2D_DESC desc;
      texture2D->GetDesc(&desc);
      if (desc.Format != DXGI_FORMAT_NV12) {
        LOG_ERROR(std::string("convert: texture format mismatch, ") +
                  std::to_string(desc.Format) +
                  " != " + std::to_string(DXGI_FORMAT_NV12));
        return false;
      }
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
      if (!native_->BgraToNv12((ID3D11Texture2D *)texture, texture2D, width_,
                               height_, colorSpace_in, colorSpace_out)) {
        LOG_ERROR(std::string("convert: BgraToNv12 failed"));
        return false;
      }
      return true;
    } else {
      LOG_ERROR(std::string("convert: unsupported format, ") +
                std::to_string(frame_->format));
      return false;
    }
  }

  bool set_hwframe_ctx() {
    AVBufferRef *hw_frames_ref;
    AVHWFramesContext *frames_ctx = NULL;
    int err = 0;
    bool ret = true;

    if (!(hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx_))) {
      LOG_ERROR(std::string("av_hwframe_ctx_alloc failed."));
      return false;
    }
    frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
    frames_ctx->format = encoder_->hw_pixfmt_;
    frames_ctx->sw_format = encoder_->sw_pixfmt_;
    frames_ctx->width = width_;
    frames_ctx->height = height_;
    frames_ctx->initial_pool_size = 0;
    if (encoder_->device_type_ == AV_HWDEVICE_TYPE_D3D11VA) {
      frames_ctx->initial_pool_size = 1;
      AVD3D11VAFramesContext *frames_hwctx =
          (AVD3D11VAFramesContext *)frames_ctx->hwctx;
      frames_hwctx->BindFlags = D3D11_BIND_RENDER_TARGET;
      frames_hwctx->MiscFlags = 0;
    }
    if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0) {
      LOG_ERROR(std::string("av_hwframe_ctx_init failed."));
      av_buffer_unref(&hw_frames_ref);
      return false;
    }
    c_->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!c_->hw_frames_ctx) {
      LOG_ERROR(std::string("av_buffer_ref failed"));
      ret = false;
    }
    av_buffer_unref(&hw_frames_ref);

    return ret;
  }
};

void lockContext(void *lock_ctx) { (void)lock_ctx; }

void unlockContext(void *lock_ctx) { (void)lock_ctx; }

} // namespace

extern "C" {
FFmpegVRamEncoder *ffmpeg_vram_new_encoder(void *handle, int64_t luid,
                                           DataFormat dataFormat, int32_t width,
                                           int32_t height, int32_t kbs,
                                           int32_t framerate, int32_t gop,
                                           int quality, int rc, int q,
                                           int spatial_aq, int temporal_aq,
                                           int multipass, int preanalysis) {
  FFmpegVRamEncoder *encoder = NULL;
  try {
    encoder = new FFmpegVRamEncoder(handle, luid, dataFormat, width,
                                    height, kbs, framerate, gop, quality, rc,
                                    q, spatial_aq, temporal_aq, multipass,
                                    preanalysis);
    if (encoder) {
      if (encoder->init()) {
        return encoder;
      }
    }
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("new FFmpegVRamEncoder failed, ") + std::string(e.what()));
  }
  if (encoder) {
    encoder->destroy();
    delete encoder;
    encoder = NULL;
  }
  return NULL;
}

int ffmpeg_vram_encode(FFmpegVRamEncoder *encoder, void *texture,
                       EncodeCallback callback, void *obj, int64_t ms) {
  try {
    return encoder->encode(texture, callback, obj, ms);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("ffmpeg_vram_encode failed, ") + std::string(e.what()));
  }
  return -1;
}

void ffmpeg_vram_destroy_encoder(FFmpegVRamEncoder *encoder) {
  try {
    if (!encoder)
      return;
    encoder->destroy();
    delete encoder;
    encoder = NULL;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("free encoder failed, ") + std::string(e.what()));
  }
}

int ffmpeg_vram_set_bitrate(FFmpegVRamEncoder *encoder, int kbs) {
  try {
    return encoder->set_bitrate(kbs);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("ffmpeg_ram_set_bitrate failed, ") + std::string(e.what()));
  }
  return -1;
}

int ffmpeg_vram_set_framerate(FFmpegVRamEncoder *encoder, int32_t framerate) {
  try {
    return encoder->set_framerate(framerate);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("ffmpeg_vram_set_framerate failed, ") + std::string(e.what()));
  }
  return -1;
}

int ffmpeg_vram_test_encode(int64_t *outLuids, int32_t *outVendors, int32_t maxDescNum,
                            int32_t *outDescNum, DataFormat dataFormat,
                            int32_t width, int32_t height, int32_t kbs,
                            int32_t framerate, int32_t gop,
                            const int64_t *excludedLuids, const int32_t *excludeFormats, int32_t excludeCount) {
  try {
    int count = 0;
    struct VendorMapping {
       AdapterVendor adapter_vendor;
       int driver_vendor;
    };
    VendorMapping vendors[] = {
      {ADAPTER_VENDOR_INTEL, VENDOR_INTEL},
      {ADAPTER_VENDOR_NVIDIA, VENDOR_NV},
      {ADAPTER_VENDOR_AMD, VENDOR_AMD}
    };
    
    for (auto vendorMap : vendors) {
      Adapters adapters;
      if (!adapters.Init(vendorMap.adapter_vendor))
        continue;
      for (auto &adapter : adapters.adapters_) {
        int64_t currentLuid = LUID(adapter.get()->desc1_);
        if (util::skip_test(excludedLuids, excludeFormats, excludeCount, currentLuid, dataFormat)) {
          continue;
        }
        
        FFmpegVRamEncoder *e = (FFmpegVRamEncoder *)ffmpeg_vram_new_encoder(
            (void *)adapter.get()->device_.Get(), currentLuid,
            dataFormat, width, height, kbs, framerate, gop,
            Quality_Default, RC_CBR, -1, 0, 0, 0, 0);
        if (!e)
          continue;
        if (e->native_->EnsureTexture(e->width_, e->height_)) {
          e->native_->next();
          int32_t key_obj = 0;
          auto start = util::now();
          bool succ = ffmpeg_vram_encode(e, e->native_->GetCurrentTexture(), util_encode::vram_encode_test_callback,
                                 &key_obj, 0) == 0 && key_obj == 1;
          int64_t elapsed = util::elapsed_ms(start);
          if (succ && elapsed < TEST_TIMEOUT_MS) {
            outLuids[count] = currentLuid;
            outVendors[count] = (int32_t)vendorMap.driver_vendor;  // Map adapter vendor to driver vendor
            count += 1;
          }
        }
        e->destroy();
        delete e;
        e = nullptr;
        if (count >= maxDescNum)
          break;
      }
      if (count >= maxDescNum)
        break;
    }
    *outDescNum = count;
    return 0;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("test failed: ") + e.what());
  }
  return -1;
}

} // extern "C"
