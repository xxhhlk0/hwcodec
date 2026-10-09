// https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/hw_decode.c
// https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/decode_video.c

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <cstring>
#include <memory>
#include <stdbool.h>
#include <vector>

#define LOG_MODULE "FFMPEG_RAM_DEC"
#include <annexb.h>
#include <log.h>
#include <util.h>

#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif

#include "common.h"
#include "system.h"

// #define CFG_PKG_TRACE

namespace {
typedef void (*RamDecodeCallback)(const void *obj, int width, int height,
                                  enum AVPixelFormat pixfmt,
                                  int linesize[AV_NUM_DATA_POINTERS],
                                  uint8_t *data[AV_NUM_DATA_POINTERS], int key);

class FFmpegRamDecoder {
public:
  // 参数集/尺寸最多等多少个包 (≈1s@30fps); 超了就走软解回退。
  static const int MAX_DEFER_COUNT = 30;

  AVCodecContext *c_ = NULL;
  AVBufferRef *hw_device_ctx_ = NULL;
  AVFrame *sw_frame_ = NULL;
  AVFrame *frame_ = NULL;
  AVPacket *pkt_ = NULL;
  bool hwaccel_ = true;

  std::string name_;
  AVHWDeviceType device_type_ = AV_HWDEVICE_TYPE_NONE;
  int thread_count_ = 1;
  RamDecodeCallback callback_ = NULL;
  DataFormat data_format_;

  // mediacodec wrapper 在 open 阶段就要求 extradata 非空, 而参数集只能从码流
  // 首包里取 (见 annexb.h), 所以它走延迟 open: reset() 只建上下文, 首次
  // decode() 拿到参数集后再 open。
  const AVCodec *codec_ = NULL;
  bool opened_ = false;
  bool open_failed_ = false;
  bool got_vps_ = false;
  bool got_sps_ = false;
  bool got_pps_ = false;
  std::vector<uint8_t> extradata_;
  // 跨包复用的 parser: mediacodec 的 configure 尺寸只能从码流里取 (见
  // probe_dimensions), 而参数集与首个 slice 未必在同一个包里, parser 得留着
  // 累积状态。
  AVCodecParserContext *parser_ = NULL;
  // 参数集/尺寸还没凑齐的包数, 超过上限就放弃, 交上层回退软解。
  int defer_count_ = 0;

#ifdef CFG_PKG_TRACE
  int in_ = 0;
  int out_ = 0;
#endif

  FFmpegRamDecoder(const char *name, int device_type, int thread_count,
                   RamDecodeCallback callback) {
    this->name_ = name;
    this->device_type_ = (AVHWDeviceType)device_type;
    this->thread_count_ = thread_count;
    this->callback_ = callback;
  }

  ~FFmpegRamDecoder() {}

  void free_decoder() {
    if (frame_)
      av_frame_free(&frame_);
    if (pkt_)
      av_packet_free(&pkt_);
    if (sw_frame_)
      av_frame_free(&sw_frame_);
    if (c_)
      avcodec_free_context(&c_);
    if (hw_device_ctx_)
      av_buffer_unref(&hw_device_ctx_);
    if (parser_)
      av_parser_close(parser_);

    frame_ = NULL;
    pkt_ = NULL;
    sw_frame_ = NULL;
    c_ = NULL;
    hw_device_ctx_ = NULL;
    parser_ = NULL;
  }
  int reset() {
    if (name_.find("h264") != std::string::npos) {
      data_format_ = DataFormat::H264;
    } else if (name_.find("hevc") != std::string::npos) {
      data_format_ = DataFormat::H265;
    } else {
      LOG_ERROR(std::string("unsupported data format:") + name_);
      return -1;
    }
    free_decoder();
    opened_ = false;
    open_failed_ = false;
    got_vps_ = false;
    got_sps_ = false;
    got_pps_ = false;
    extradata_.clear();
    defer_count_ = 0;
    hwaccel_ = device_type_ != AV_HWDEVICE_TYPE_NONE;
    int ret;
    if (!(codec_ = avcodec_find_decoder_by_name(name_.c_str()))) {
      LOG_ERROR(std::string("avcodec_find_decoder_by_name ") + name_ + " failed");
      return -1;
    }
    if (!(c_ = avcodec_alloc_context3(codec_))) {
      LOG_ERROR(std::string("Could not allocate video codec context"));
      return -1;
    }

    c_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    c_->thread_count =
        device_type_ != AV_HWDEVICE_TYPE_NONE ? 1 : thread_count_;
    c_->thread_type = FF_THREAD_SLICE;

    if (name_.find("qsv") != std::string::npos) {
      if ((ret = av_opt_set(c_->priv_data, "async_depth", "1", 0)) < 0) {
        LOG_ERROR(std::string("qsv set opt async_depth 1 failed"));
        return -1;
      }
      // https://github.com/FFmpeg/FFmpeg/blob/c6364b711bad1fe2fbd90e5b2798f87080ddf5ea/libavcodec/qsvdec.c#L932
      // for disable warning
      c_->pkt_timebase = av_make_q(1, 30);
    }

    if (hwaccel_) {
      ret =
          av_hwdevice_ctx_create(&hw_device_ctx_, device_type_, NULL, NULL, 0);
      if (ret < 0) {
        LOG_ERROR(std::string("av_hwdevice_ctx_create failed, ret = ") + av_err2str(ret));
        return -1;
      }
      c_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
      if (!check_support()) {
        LOG_ERROR(std::string("check_support failed"));
        return -1;
      }
      if (!(sw_frame_ = av_frame_alloc())) {
        LOG_ERROR(std::string("av_frame_alloc failed"));
        return -1;
      }
    }

    if (!(pkt_ = av_packet_alloc())) {
      LOG_ERROR(std::string("av_packet_alloc failed"));
      return -1;
    }

    if (!(frame_ = av_frame_alloc())) {
      LOG_ERROR(std::string("av_frame_alloc failed"));
      return -1;
    }

    // 见成员注释: mediacodec 延迟到首次 decode() 再 open。
    const bool defer_open = name_.find("mediacodec") != std::string::npos;
    if (!defer_open && (ret = avcodec_open2(c_, codec_, NULL)) != 0) {
      LOG_ERROR(std::string("avcodec_open2 failed, ret = ") + av_err2str(ret));
      return -1;
    }
    opened_ = !defer_open;
#ifdef CFG_PKG_TRACE
    in_ = 0;
    out_ = 0;
#endif

    return 0;
  }

  int decode(const uint8_t *data, int length, const void *obj) {
    int ret = -1;
#ifdef CFG_PKG_TRACE
    in_++;
    LOG_DEBUG(std::string("delay DI: in:") + in_ + " out:" + out_);
#endif

    if (!data || !length) {
      LOG_ERROR(std::string("illegal decode parameter"));
      return -1;
    }
    if (!opened_) {
      ret = open_with_extradata(data, length);
      if (ret <= 0) {
        // 0: 本包还凑不齐参数集/尺寸, 静默等下一包 —— 返回成功且无帧, 免得
        //    上层把"还没准备好"当成解码失败而回退软解。
        // -1: 放弃, 交上层回退软解。
        return ret;
      }
    }
    pkt_->data = (uint8_t *)data;
    pkt_->size = length;
    ret = do_decode(obj);
    return ret;
  }

private:
  // ffmpeg 的 mediacodec wrapper 在 open 时用 avctx->width/height 去 configure
  // (libavcodec/mediacodecdec.c: ff_AMediaFormat_setInt32(format, "width", ...)),
  // 给 0 会被 MediaCodec 直接拒掉 —— Qualcomm c2 报
  // "Failed to configure codec c2.qti.avc.decoder ... width=0, height=0"。
  // 参数集里没有尺寸字段可读, 但 Android 的 ffmpeg 编了 h264/hevc parser
  // (portfile.cmake: --enable-parser=h264,hevc), 拿 parser 过一遍码流即可。
  // 取 coded 尺寸: MediaCodec 的 width/height 描述的是解码缓冲区, 得 >= coded,
  // 显示尺寸由设备上报的 crop 修正 (mediacodecdec_common.c 读 crop-* 后
  // ff_set_dimensions); ExoPlayer 同样传 coded 尺寸。
  bool probe_dimensions(const uint8_t *data, int length) {
    if (!parser_) {
      parser_ = av_parser_init(data_format_ == DataFormat::H264
                                   ? AV_CODEC_ID_H264
                                   : AV_CODEC_ID_HEVC);
      if (!parser_) {
        LOG_ERROR(std::string("av_parser_init failed for ") + name_);
        return false;
      }
    }
    uint8_t *out = NULL;
    int out_size = 0;
    av_parser_parse2(parser_, c_, &out, &out_size, data, length, AV_NOPTS_VALUE,
                     AV_NOPTS_VALUE, 0);
    // 再空喂一次 = flush。parser 会缓存数据等下一个 AU 的起始码来定边界
    // (libavcodec/parser.c: !*buf_size && next==END_NOT_FOUND -> next=0),
    // 不 flush 就一直不解析。flush 只清缓冲, 参数集状态留着。
    av_parser_parse2(parser_, c_, &out, &out_size, NULL, 0, AV_NOPTS_VALUE,
                     AV_NOPTS_VALUE, 0);
    int w = parser_->coded_width > 0 ? parser_->coded_width : parser_->width;
    int h = parser_->coded_height > 0 ? parser_->coded_height : parser_->height;
    if (w <= 0 || h <= 0) {
      return false;
    }
    c_->width = w;
    c_->height = h;
    LOG_INFO(std::string("probed frame size ") + std::to_string(w) + "x" +
             std::to_string(h) + " for " + name_);
    return true;
  }

  // 用码流里的参数集补上 extradata 再 open (仅延迟 open 的解码器会走到)。
  // 返回值: 1 = 已 open; 0 = 本包不够, 等下一包; -1 = 放弃 (置 open_failed_)。
  int open_with_extradata(const uint8_t *data, int length) {
    if (open_failed_) {
      return -1;
    }
    if (util_decode::collect_parameter_sets(data, length, data_format_,
                                            extradata_, got_vps_, got_sps_,
                                            got_pps_)) {
      if (!c_->extradata) {
        c_->extradata = (uint8_t *)av_mallocz(extradata_.size() +
                                              AV_INPUT_BUFFER_PADDING_SIZE);
        if (!c_->extradata) {
          LOG_ERROR(std::string("av_mallocz extradata failed"));
          open_failed_ = true;
          return -1;
        }
        memcpy(c_->extradata, extradata_.data(), extradata_.size());
        c_->extradata_size = (int)extradata_.size();
      }
      // mediacodec 还必须在 open 前给出尺寸, 否则 configure 必失败。
      if (probe_dimensions(data, length)) {
        int ret = avcodec_open2(c_, codec_, NULL);
        if (ret < 0) {
          LOG_ERROR(std::string("avcodec_open2 failed, ret = ") + av_err2str(ret));
          open_failed_ = true;
          return -1;
        }
        opened_ = true;
        LOG_INFO(std::string("opened ") + name_ + " with extradata " +
                 std::to_string(extradata_.size()) + " bytes");
        return 1;
      }
    }
    if (defer_count_++ == 0) {
      LOG_WARN(std::string("parameter set or frame size not ready, wait for the "
                           "next packet: ") +
               name_);
    }
    // 参数集可能被拆到后面的包里, 也可能要等下一个 IDR 才带上; 但也不能无限
    // 等下去 (否则永远没有帧), 超上限就放弃, 由上层回退软解。
    if (defer_count_ > MAX_DEFER_COUNT) {
      LOG_ERROR(std::string("give up waiting for parameter set/frame size: ") +
                name_);
      open_failed_ = true;
      return -1;
    }
    return 0;
  }

  int do_decode(const void *obj) {
    int ret;
    AVFrame *tmp_frame = NULL;
    bool decoded = false;

    ret = avcodec_send_packet(c_, pkt_);
    if (ret < 0) {
      LOG_ERROR(std::string("avcodec_send_packet failed, ret = ") + av_err2str(ret));
      return ret;
    }
    auto start = util::now();
    while (ret >= 0 && util::elapsed_ms(start) < ENCODE_TIMEOUT_MS) {
      if ((ret = avcodec_receive_frame(c_, frame_)) != 0) {
        if (ret != AVERROR(EAGAIN)) {
          LOG_ERROR(std::string("avcodec_receive_frame failed, ret = ") + av_err2str(ret));
        }
        goto _exit;
      }

      if (hwaccel_) {
        if (!frame_->hw_frames_ctx) {
          LOG_ERROR(std::string("hw_frames_ctx is NULL"));
          goto _exit;
        }
        if ((ret = av_hwframe_transfer_data(sw_frame_, frame_, 0)) < 0) {
          LOG_ERROR(std::string("av_hwframe_transfer_data failed, ret = ") +
                    av_err2str(ret));
          goto _exit;
        }

        tmp_frame = sw_frame_;
      } else {
        tmp_frame = frame_;
      }
      decoded = true;
#ifdef CFG_PKG_TRACE
      out_++;
      LOG_DEBUG(std::string("delay DO: in:") + in_ + " out:" + out_);
#endif
#if FF_API_FRAME_KEY
      int key_frame = frame_->flags & AV_FRAME_FLAG_KEY;
#else
      int key_frame = frame_->key_frame;
#endif

      callback_(obj, tmp_frame->width, tmp_frame->height,
                (AVPixelFormat)tmp_frame->format, tmp_frame->linesize,
                tmp_frame->data, key_frame);
    }
  _exit:
    av_packet_unref(pkt_);
    return decoded ? 0 : -1;
  }

  bool check_support() {
#ifdef _WIN32
    if (device_type_ == AV_HWDEVICE_TYPE_D3D11VA) {
      if (!c_->hw_device_ctx) {
        LOG_ERROR(std::string("hw_device_ctx is NULL"));
        return false;
      }
      AVHWDeviceContext *deviceContext =
          (AVHWDeviceContext *)hw_device_ctx_->data;
      if (!deviceContext) {
        LOG_ERROR(std::string("deviceContext is NULL"));
        return false;
      }
      AVD3D11VADeviceContext *d3d11vaDeviceContext =
          (AVD3D11VADeviceContext *)deviceContext->hwctx;
      if (!d3d11vaDeviceContext) {
        LOG_ERROR(std::string("d3d11vaDeviceContext is NULL"));
        return false;
      }
      ID3D11Device *device = d3d11vaDeviceContext->device;
      if (!device) {
        LOG_ERROR(std::string("device is NULL"));
        return false;
      }
      std::unique_ptr<NativeDevice> native_ = std::make_unique<NativeDevice>();
      if (!native_) {
        LOG_ERROR(std::string("Failed to create native device"));
        return false;
      }
      if (!native_->Init(0, (ID3D11Device *)device, 0)) {
        LOG_ERROR(std::string("Failed to init native device"));
        return false;
      }
      if (!native_->support_decode(data_format_)) {
        LOG_ERROR(std::string("Failed to check support ") + name_);
        return false;
      }
      return true;
    } else {
      return true;
    }
#else
    return true;
#endif
  }
};

} // namespace

extern "C" void ffmpeg_ram_free_decoder(FFmpegRamDecoder *decoder) {
  try {
    if (!decoder)
      return;
    decoder->free_decoder();
    delete decoder;
    decoder = NULL;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("ffmpeg_ram_free_decoder exception:") + e.what());
  }
}

extern "C" FFmpegRamDecoder *
ffmpeg_ram_new_decoder(const char *name, int device_type, int thread_count,
                       RamDecodeCallback callback) {
  FFmpegRamDecoder *decoder = NULL;
  try {
    decoder = new FFmpegRamDecoder(name, device_type, thread_count, callback);
    if (decoder) {
      if (decoder->reset() == 0) {
        return decoder;
      }
    }
  } catch (std::exception &e) {
    LOG_ERROR(std::string("new decoder exception:") + e.what());
  }
  if (decoder) {
    decoder->free_decoder();
    delete decoder;
    decoder = NULL;
  }
  return NULL;
}

extern "C" int ffmpeg_ram_decode(FFmpegRamDecoder *decoder, const uint8_t *data,
                                 int length, const void *obj) {
  try {
    int ret = decoder->decode(data, length, obj);
    if (DataFormat::H265 == decoder->data_format_ && util_decode::has_flag_could_not_find_ref_with_poc()) {
      return HWCODEC_ERR_HEVC_COULD_NOT_FIND_POC;
    } else {
      return ret == 0 ? HWCODEC_SUCCESS : HWCODEC_ERR_COMMON;
    }
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("ffmpeg_ram_decode exception:") + e.what());
  }
  return HWCODEC_ERR_COMMON;
}
