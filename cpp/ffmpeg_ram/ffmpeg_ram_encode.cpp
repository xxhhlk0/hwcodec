// https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/encode_video.c

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
}

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

#define LOG_MODULE "FFMPEG_RAM_ENC"
#include <log.h>
#include <util.h>
#ifdef _WIN32
#include "win.h"
#endif

static int calculate_offset_length(int pix_fmt, int height, const int *linesize,
                                   int *offset, int *length) {
  switch (pix_fmt) {
  case AV_PIX_FMT_YUV420P:
    offset[0] = linesize[0] * height;
    offset[1] = offset[0] + linesize[1] * height / 2;
    *length = offset[1] + linesize[2] * height / 2;
    break;
  case AV_PIX_FMT_NV12:
    offset[0] = linesize[0] * height;
    *length = offset[0] + linesize[1] * height / 2;
    break;
  default:
    LOG_ERROR(std::string("unsupported pixfmt") + std::to_string(pix_fmt));
    return -1;
  }

  return 0;
}

extern "C" int ffmpeg_ram_get_linesize_offset_length(int pix_fmt, int width,
                                                     int height, int align,
                                                     int *linesize, int *offset,
                                                     int *length) {
  AVFrame *frame = NULL;
  int ioffset[AV_NUM_DATA_POINTERS] = {0};
  int ilength = 0;
  int ret = -1;

  if (!(frame = av_frame_alloc())) {
    LOG_ERROR(std::string("Alloc frame failed"));
    goto _exit;
  }

  frame->format = pix_fmt;
  frame->width = width;
  frame->height = height;

  if ((ret = av_frame_get_buffer(frame, align)) < 0) {
    LOG_ERROR(std::string("av_frame_get_buffer, ret = ") + av_err2str(ret));
    goto _exit;
  }
  if (linesize) {
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++)
      linesize[i] = frame->linesize[i];
  }
  if (offset || length) {
    ret = calculate_offset_length(pix_fmt, height, frame->linesize, ioffset,
                                  &ilength);
    if (ret < 0)
      goto _exit;
  }
  if (offset) {
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
      if (ioffset[i] == 0)
        break;
      offset[i] = ioffset[i];
    }
  }
  if (length)
    *length = ilength;

  ret = 0;
_exit:
  if (frame)
    av_frame_free(&frame);
  return ret;
}

namespace {
typedef void (*RamEncodeCallback)(const uint8_t *data, int len, int64_t pts,
                                  int key, const void *obj);

class FFmpegRamEncoder {
public:
  AVCodecContext *c_ = NULL;
  AVFrame *frame_ = NULL;
  AVPacket *pkt_ = NULL;
  std::string name_;
  std::string mc_name_; // for mediacodec

  int width_ = 0;
  int height_ = 0;
  AVPixelFormat pixfmt_ = AV_PIX_FMT_NV12;
  int align_ = 0;
  int rc_ = 0;
  int quality_ = 0;
  int kbs_ = 0;
  int q_ = 0;
  int spatial_aq_ = 0;
  int temporal_aq_ = 0;
  int multipass_ = 0;
  int preanalysis_ = 0;
  // 是否应用过画质增强项 (avcodec_open2 失败时据此去掉增强项重试一次)
  bool enhance_applied_ = false;
  int fps_ = 30;
  int gop_ = 0xFFFF;
  int thread_count_ = 1;
  int gpu_ = 0;
  RamEncodeCallback callback_ = NULL;
  int offset_[AV_NUM_DATA_POINTERS] = {0};

  AVHWDeviceType hw_device_type_ = AV_HWDEVICE_TYPE_NONE;
  AVPixelFormat hw_pixfmt_ = AV_PIX_FMT_NONE;
  AVBufferRef *hw_device_ctx_ = NULL;
  AVFrame *hw_frame_ = NULL;

  FFmpegRamEncoder(const char *name, const char *mc_name, int width, int height,
                   int pixfmt, int align, int fps, int gop, int rc, int quality,
                   int kbs, int q, int spatial_aq, int temporal_aq,
                   int multipass, int preanalysis, int thread_count, int gpu,
                   RamEncodeCallback callback) {
    name_ = name;
    mc_name_ = mc_name ? mc_name : "";
    width_ = width;
    height_ = height;
    pixfmt_ = (AVPixelFormat)pixfmt;
    align_ = align;
    fps_ = fps;
    gop_ = gop;
    rc_ = rc;
    quality_ = quality;
    kbs_ = kbs;
    q_ = q;
    spatial_aq_ = spatial_aq;
    temporal_aq_ = temporal_aq;
    multipass_ = multipass;
    preanalysis_ = preanalysis;
    thread_count_ = thread_count;
    gpu_ = gpu;
    callback_ = callback;
    if (name_.find("vaapi") != std::string::npos) {
      hw_device_type_ = AV_HWDEVICE_TYPE_VAAPI;
      hw_pixfmt_ = AV_PIX_FMT_VAAPI;
    } else if (name_.find("nvenc") != std::string::npos) {
#ifdef _WIN32
      hw_device_type_ = AV_HWDEVICE_TYPE_D3D11VA;
      hw_pixfmt_ = AV_PIX_FMT_D3D11;
#endif
    }
  }

  ~FFmpegRamEncoder() {}

  bool init(int *linesize, int *offset, int *length) {
    const AVCodec *codec = NULL;

    int ret;

    if (!(codec = avcodec_find_encoder_by_name(name_.c_str()))) {
      LOG_ERROR(std::string("Codec ") + name_ + " not found");
      return false;
    }

    if (!(c_ = avcodec_alloc_context3(codec))) {
      LOG_ERROR(std::string("Could not allocate video codec context"));
      return false;
    }

    if (hw_device_type_ != AV_HWDEVICE_TYPE_NONE) {
      std::string device = "";
#ifdef _WIN32
      if (name_.find("nvenc") != std::string::npos) {
        int index = Adapters::GetFirstAdapterIndex(
            AdapterVendor::ADAPTER_VENDOR_NVIDIA);
        if (index >= 0) {
          device = std::to_string(index);
        }
      }
#endif
      ret = av_hwdevice_ctx_create(&hw_device_ctx_, hw_device_type_,
                                   device.length() == 0 ? NULL : device.c_str(),
                                   NULL, 0);
      if (ret < 0) {
        LOG_ERROR(std::string("av_hwdevice_ctx_create failed"));
        return false;
      }
      if (set_hwframe_ctx() != 0) {
        LOG_ERROR(std::string("set_hwframe_ctx failed"));
        return false;
      }
      hw_frame_ = av_frame_alloc();
      if (!hw_frame_) {
        LOG_ERROR(std::string("av_frame_alloc failed"));
        return false;
      }
      if ((ret = av_hwframe_get_buffer(c_->hw_frames_ctx, hw_frame_, 0)) < 0) {
        LOG_ERROR(std::string("av_hwframe_get_buffer failed, ret = ") + av_err2str(ret));
        return false;
      }
      if (!hw_frame_->hw_frames_ctx) {
        LOG_ERROR(std::string("hw_frame_->hw_frames_ctx is NULL"));
        return false;
      }
    }

    if (!(frame_ = av_frame_alloc())) {
      LOG_ERROR(std::string("Could not allocate video frame"));
      return false;
    }
    frame_->format = pixfmt_;
    frame_->width = width_;
    frame_->height = height_;

    if ((ret = av_frame_get_buffer(frame_, align_)) < 0) {
      LOG_ERROR(std::string("av_frame_get_buffer failed, ret = ") + av_err2str(ret));
      return false;
    }

    if (!(pkt_ = av_packet_alloc())) {
      LOG_ERROR(std::string("Could not allocate video packet"));
      return false;
    }

    /* resolution must be a multiple of two */
    c_->width = width_;
    c_->height = height_;
    c_->pix_fmt =
        hw_pixfmt_ != AV_PIX_FMT_NONE ? hw_pixfmt_ : (AVPixelFormat)pixfmt_;
    c_->sw_pix_fmt = (AVPixelFormat)pixfmt_;
    util_encode::set_av_codec_ctx(c_, name_, kbs_, gop_, fps_);
    if (!util_encode::set_lantency_free(c_->priv_data, name_)) {
      LOG_ERROR(std::string("set_lantency_free failed, name: ") + name_);
      return false;
    }
    // preset/quality: previously commented out, so the quality argument passed in from
    // rustdesk (encode profile) had no effect at all. Quality_Default is a no-op, so this
    // only changes behaviour when a non-default preset is explicitly requested.
    if (!util_encode::set_quality(c_->priv_data, name_, quality_)) {
      // do not fail the session, just keep the encoder default preset
      LOG_ERROR(std::string("set_quality failed, keep the default preset, name: ") + name_);
    }
    util_encode::set_rate_control(c_, name_, rc_, q_);
    enhance_applied_ = util_encode::set_encode_enhance(
        c_->priv_data, name_, spatial_aq_, temporal_aq_, multipass_,
        preanalysis_);
    util_encode::set_gpu(c_->priv_data, name_, gpu_);
    util_encode::force_hw(c_->priv_data, name_);
    util_encode::set_others(c_->priv_data, name_);
    // 打印请求参数 + 解码器实际拿到的码控字段, 便于确认 rc/preset/QP 是否真的生效
    // (例如 qsv + rc=CQ 时 bit_rate/rc_max_rate 应为 0, global_quality = q -> ffmpeg 选 ICQ)
    LOG_INFO("hw encode params: name=" + name_ +
             ", quality=" + std::to_string(quality_) + ", rc=" +
             std::to_string(rc_) + ", q=" + std::to_string(q_) +
             ", kbs=" + std::to_string(kbs_) + ", fps=" + std::to_string(fps_) +
             ", gop=" + std::to_string(gop_) +
             ", bit_rate=" + std::to_string(c_->bit_rate) +
             ", rc_max_rate=" + std::to_string(c_->rc_max_rate) +
             ", global_quality=" + std::to_string(c_->global_quality));
    if (name_.find("mediacodec") != std::string::npos) {
      if (mc_name_.length() > 0) {
        LOG_INFO(std::string("mediacodec codec_name: ") + mc_name_);
        if ((ret = av_opt_set(c_->priv_data, "codec_name", mc_name_.c_str(),
                              0)) < 0) {
          LOG_ERROR(std::string("mediacodec codec_name failed, ret = ") + av_err2str(ret));
        }
      }
    }

    int open_ret = avcodec_open2(c_, codec, NULL);
    if (open_ret < 0 && enhance_applied_) {
      // 画质增强是可选项: 部分 GPU/驱动会拒绝 (例如 nvenc 的 temporal AQ 能力检查
      // 返回 ENOSYS, amf 不支持 preanalysis 属性)。此时去掉增强项重试一次,
      // 而不是让远程会话建不起来。
      LOG_WARN(std::string("avcodec_open2 failed with encode enhancement, retry "
                           "without them, ret = ") +
               av_err2str(open_ret) + ", name: " + name_);
      util_encode::set_encode_enhance(c_->priv_data, name_, 0, 0, 0, 0);
      enhance_applied_ = false;
      open_ret = avcodec_open2(c_, codec, NULL);
    }
    if (open_ret < 0) {
      LOG_ERROR(std::string("avcodec_open2 failed, ret = ") + av_err2str(open_ret) +
                ", name: " + name_);
      return false;
    }
    // open 之后 ffmpeg 才真正选定码控模式, 并可能回填这些字段;
    // 排查"参数到底生效没有"以这一行为准 (与上面 hw encode params 对比)。
    LOG_INFO("hw encode opened: name=" + name_ +
             ", bit_rate=" + std::to_string(c_->bit_rate) +
             ", rc_max_rate=" + std::to_string(c_->rc_max_rate) +
             ", global_quality=" + std::to_string(c_->global_quality));

    if (ffmpeg_ram_get_linesize_offset_length(pixfmt_, width_, height_, align_,
                                              NULL, offset_, length) != 0)
      return false;

    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
      linesize[i] = frame_->linesize[i];
      offset[i] = offset_[i];
    }
    return true;
  }

  int encode(const uint8_t *data, int length, const void *obj, uint64_t ms) {
    int ret;

    if ((ret = av_frame_make_writable(frame_)) != 0) {
      LOG_ERROR(std::string("av_frame_make_writable failed, ret = ") + av_err2str(ret));
      return ret;
    }
    if ((ret = fill_frame(frame_, (uint8_t *)data, length, offset_)) != 0)
      return ret;
    AVFrame *tmp_frame;
    if (hw_device_type_ != AV_HWDEVICE_TYPE_NONE) {
      if ((ret = av_hwframe_transfer_data(hw_frame_, frame_, 0)) < 0) {
        LOG_ERROR(std::string("av_hwframe_transfer_data failed, ret = ") + av_err2str(ret));
        return ret;
      }
      tmp_frame = hw_frame_;
    } else {
      tmp_frame = frame_;
    }

    return do_encode(tmp_frame, obj, ms);
  }

  void free_encoder() {
    if (pkt_)
      av_packet_free(&pkt_);
    if (frame_)
      av_frame_free(&frame_);
    if (hw_frame_)
      av_frame_free(&hw_frame_);
    if (hw_device_ctx_)
      av_buffer_unref(&hw_device_ctx_);
    if (c_)
      avcodec_free_context(&c_);
  }

  int set_bitrate(int kbs) {
    return util_encode::change_bit_rate(c_, name_, kbs) ? 0 : -1;
  }

private:
  int set_hwframe_ctx() {
    AVBufferRef *hw_frames_ref;
    AVHWFramesContext *frames_ctx = NULL;
    int err = 0;

    if (!(hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx_))) {
      LOG_ERROR(std::string("av_hwframe_ctx_alloc failed"));
      return -1;
    }
    frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
    frames_ctx->format = hw_pixfmt_;
    frames_ctx->sw_format = (AVPixelFormat)pixfmt_;
    frames_ctx->width = width_;
    frames_ctx->height = height_;
    frames_ctx->initial_pool_size = 1;
    if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0) {
      av_buffer_unref(&hw_frames_ref);
      return err;
    }
    c_->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!c_->hw_frames_ctx) {
      LOG_ERROR(std::string("av_buffer_ref failed"));
      err = -1;
    }
    av_buffer_unref(&hw_frames_ref);
    return err;
  }

  int do_encode(AVFrame *frame, const void *obj, int64_t ms) {
    int ret;
    bool encoded = false;
    frame->pts = ms;
    if ((ret = avcodec_send_frame(c_, frame)) < 0) {
      LOG_ERROR(std::string("avcodec_send_frame failed, ret = ") + av_err2str(ret));
      return ret;
    }

    auto start = util::now();
    while (ret >= 0 && util::elapsed_ms(start) < DECODE_TIMEOUT_MS) {
      if ((ret = avcodec_receive_packet(c_, pkt_)) < 0) {
        if (ret != AVERROR(EAGAIN)) {
          LOG_ERROR(std::string("avcodec_receive_packet failed, ret = ") + av_err2str(ret));
        }
        goto _exit;
      }
      if (!pkt_->data || !pkt_->size) {
        LOG_ERROR(std::string("avcodec_receive_packet failed, pkt size is 0"));
        goto _exit;
      }
      encoded = true;
      callback_(pkt_->data, pkt_->size, pkt_->pts,
                pkt_->flags & AV_PKT_FLAG_KEY, obj);
    }
  _exit:
    av_packet_unref(pkt_);
    return encoded ? 0 : -1;
  }

  int fill_frame(AVFrame *frame, uint8_t *data, int data_length,
                 const int *const offset) {
    switch (frame->format) {
    case AV_PIX_FMT_NV12:
      if (data_length <
          frame->height * (frame->linesize[0] + frame->linesize[1] / 2)) {
        LOG_ERROR(std::string("fill_frame: NV12 data length error. data_length:") +
                  std::to_string(data_length) +
                  ", linesize[0]:" + std::to_string(frame->linesize[0]) +
                  ", linesize[1]:" + std::to_string(frame->linesize[1]));
        return -1;
      }
      frame->data[0] = data;
      frame->data[1] = data + offset[0];
      break;
    case AV_PIX_FMT_YUV420P:
      if (data_length <
          frame->height * (frame->linesize[0] + frame->linesize[1] / 2 +
                           frame->linesize[2] / 2)) {
        LOG_ERROR(std::string("fill_frame: 420P data length error. data_length:") +
                  std::to_string(data_length) +
                  ", linesize[0]:" + std::to_string(frame->linesize[0]) +
                  ", linesize[1]:" + std::to_string(frame->linesize[1]) +
                  ", linesize[2]:" + std::to_string(frame->linesize[2]));
        return -1;
      }
      frame->data[0] = data;
      frame->data[1] = data + offset[0];
      frame->data[2] = data + offset[1];
      break;
    default:
      LOG_ERROR(std::string("fill_frame: unsupported format, ") +
                std::to_string(frame->format));
      return -1;
    }
    return 0;
  }
};

} // namespace

extern "C" FFmpegRamEncoder *
ffmpeg_ram_new_encoder(const char *name, const char *mc_name, int width,
                       int height, int pixfmt, int align, int fps, int gop,
                       int rc, int quality, int kbs, int q, int spatial_aq,
                       int temporal_aq, int multipass, int preanalysis,
                       int thread_count, int gpu, int *linesize, int *offset,
                       int *length, RamEncodeCallback callback) {
  FFmpegRamEncoder *encoder = NULL;
  try {
    encoder = new FFmpegRamEncoder(name, mc_name, width, height, pixfmt, align,
                                   fps, gop, rc, quality, kbs, q, spatial_aq,
                                   temporal_aq, multipass, preanalysis,
                                   thread_count, gpu, callback);
    if (encoder) {
      if (encoder->init(linesize, offset, length)) {
        return encoder;
      }
    }
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("new FFmpegRamEncoder failed, ") + std::string(e.what()));
  }
  if (encoder) {
    encoder->free_encoder();
    delete encoder;
    encoder = NULL;
  }
  return NULL;
}

extern "C" int ffmpeg_ram_encode(FFmpegRamEncoder *encoder, const uint8_t *data,
                                 int length, const void *obj, uint64_t ms) {
  try {
    return encoder->encode(data, length, obj, ms);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("ffmpeg_ram_encode failed, ") + std::string(e.what()));
  }
  return -1;
}

extern "C" void ffmpeg_ram_free_encoder(FFmpegRamEncoder *encoder) {
  try {
    if (!encoder)
      return;
    encoder->free_encoder();
    delete encoder;
    encoder = NULL;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("free encoder failed, ") + std::string(e.what()));
  }
}

extern "C" int ffmpeg_ram_set_bitrate(FFmpegRamEncoder *encoder, int kbs) {
  try {
    return encoder->set_bitrate(kbs);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("ffmpeg_ram_set_bitrate failed, ") + std::string(e.what()));
  }
  return -1;
}