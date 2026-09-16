extern "C" {
#include <libavutil/opt.h>
}

#include "util.h"
#include <limits>
#include <map>
#include <string.h>
#include <vector>

#include "common.h"

#include "common.h"

#define LOG_MODULE "UTIL"
#include "log.h"

namespace util_encode {

void set_av_codec_ctx(AVCodecContext *c, const std::string &name, int kbs,
                      int gop, int fps) {
  c->has_b_frames = 0;
  c->max_b_frames = 0;
  if (gop > 0 && gop < std::numeric_limits<int16_t>::max()) {
    c->gop_size = gop;
  } else if (name.find("vaapi") != std::string::npos) {
    c->gop_size = std::numeric_limits<int16_t>::max();
  } else if (name.find("qsv") != std::string::npos) {
    c->gop_size = std::numeric_limits<uint16_t>::max();
  } else {
    c->gop_size = std::numeric_limits<int>::max();
  }
  c->keyint_min = std::numeric_limits<int>::max();
  /* put sample parameters */
  // https://github.com/FFmpeg/FFmpeg/blob/415f012359364a77e8394436f222b74a8641a3ee/libavcodec/encode.c#L581
  if (kbs > 0) {
    c->bit_rate = kbs * 1000;
    if (name.find("qsv") != std::string::npos) {
      c->rc_max_rate = c->bit_rate;
      c->bit_rate--; // cbr with vbr
    }
  }
  /* frames per second */
  c->time_base = av_make_q(1, 1000);
  c->framerate = av_make_q(fps, 1);
  c->flags |= AV_CODEC_FLAG2_LOCAL_HEADER;
  c->flags |= AV_CODEC_FLAG_LOW_DELAY;
  c->slices = 1;
  c->thread_type = FF_THREAD_SLICE;
  c->thread_count = c->slices;

  // https://github.com/obsproject/obs-studio/blob/3cc7dc0e7cf8b01081dc23e432115f7efd0c8877/plugins/obs-ffmpeg/obs-ffmpeg-mux.c#L160
  c->color_range = AVCOL_RANGE_MPEG;
  c->colorspace = AVCOL_SPC_SMPTE170M;
  c->color_primaries = AVCOL_PRI_SMPTE170M;
  c->color_trc = AVCOL_TRC_SMPTE170M;

  if (name.find("h264") != std::string::npos) {
    c->profile = FF_PROFILE_H264_HIGH;
  } else if (name.find("hevc") != std::string::npos) {
    c->profile = FF_PROFILE_HEVC_MAIN;
  }
}

// Intel QSV/VAAPI 的 async_depth 决定硬编码流水线里可以重叠多少帧。
// 上游为了"最低延迟"写死 1, 但这会让 iGPU 无法重叠 "取帧-编码-回读", 编码吞吐腰斩:
// 14代之前的核显 (如 UHD 750) 在 1440p 真实运动画面下实测
//   async_depth=1 -> 89fps (11.2ms/帧)
//   async_depth=2 -> 116fps (8.6ms/帧, 默认4也是同一水平)
// 代价是多 1 帧管线延迟, 60fps 下约 16ms, 对远程桌面可以接受。
#define HWCODEC_ASYNC_DEPTH 2

bool set_lantency_free(void *priv_data, const std::string &name) {
  int ret;

  if (name.find("nvenc") != std::string::npos) {
    if ((ret = av_opt_set(priv_data, "delay", "0", 0)) < 0) {
      LOG_ERROR(std::string("nvenc set_lantency_free failed, ret = ") + av_err2str(ret));
      return false;
    }
  }
  if (name.find("amf") != std::string::npos) {
    if ((ret = av_opt_set(priv_data, "query_timeout", "1000", 0)) < 0) {
      LOG_ERROR(std::string("amf set_lantency_free failed, ret = ") + av_err2str(ret));
      return false;
    }
  }
  if (name.find("qsv") != std::string::npos) {
    if ((ret = av_opt_set_int(priv_data, "async_depth", HWCODEC_ASYNC_DEPTH, 0)) < 0) {
      LOG_ERROR(std::string("qsv set_lantency_free failed, ret = ") + av_err2str(ret));
      return false;
    }
    LOG_INFO(std::string("qsv async_depth = ") +
             std::to_string(HWCODEC_ASYNC_DEPTH));
  }
  if (name.find("vaapi") != std::string::npos) {
    if ((ret = av_opt_set_int(priv_data, "async_depth", HWCODEC_ASYNC_DEPTH, 0)) < 0) {
      LOG_ERROR(std::string("vaapi set_lantency_free failed, ret = ") + av_err2str(ret));
      return false;
    }
    LOG_INFO(std::string("vaapi async_depth = ") +
             std::to_string(HWCODEC_ASYNC_DEPTH));
  }
  if (name.find("videotoolbox") != std::string::npos) {
    if ((ret = av_opt_set_int(priv_data, "realtime", 1, 0)) < 0) {
      LOG_ERROR(std::string("videotoolbox set realtime failed, ret = ") + av_err2str(ret));
      return false;
    }
    if ((ret = av_opt_set_int(priv_data, "prio_speed", 1, 0)) < 0) {
      LOG_ERROR(std::string("videotoolbox set prio_speed failed, ret = ") + av_err2str(ret));
      return false;
    }
  }
  return true;
}

bool set_quality(void *priv_data, const std::string &name, int quality) {
  int ret = -1;

  if (name.find("nvenc") != std::string::npos) {
    switch (quality) {
    // p7 isn't zero lantency, so it is only applied when the highest quality preset is
    // explicitly requested by the user; Quality_Default/Medium/Low are untouched.
    case Quality_High:
      if ((ret = av_opt_set(priv_data, "preset", "p7", 0)) < 0) {
        LOG_ERROR(std::string("nvenc set opt preset p7 failed, ret = ") + av_err2str(ret));
        return false;
      }
      break;
    case Quality_Medium:
      if ((ret = av_opt_set(priv_data, "preset", "p4", 0)) < 0) {
        LOG_ERROR(std::string("nvenc set opt preset p4 failed, ret = ") + av_err2str(ret));
        return false;
      }
      break;
    case Quality_Low:
      if ((ret = av_opt_set(priv_data, "preset", "p1", 0)) < 0) {
        LOG_ERROR(std::string("nvenc set opt preset p1 failed, ret = ") + av_err2str(ret));
        return false;
      }
      break;
    default:
      break;
    }
  }
  if (name.find("amf") != std::string::npos) {
    switch (quality) {
    case Quality_High:
      if ((ret = av_opt_set(priv_data, "quality", "quality", 0)) < 0) {
        LOG_ERROR(std::string("amf set opt quality quality failed, ret = ") +
                  av_err2str(ret));
        return false;
      }
      break;
    case Quality_Medium:
      if ((ret = av_opt_set(priv_data, "quality", "balanced", 0)) < 0) {
        LOG_ERROR(std::string("amf set opt quality balanced failed, ret = ") +
                  av_err2str(ret));
        return false;
      }
      break;
    case Quality_Low:
      if ((ret = av_opt_set(priv_data, "quality", "speed", 0)) < 0) {
        LOG_ERROR(std::string("amf set opt quality speed failed, ret = ") + av_err2str(ret));
        return false;
      }
      break;
    default:
      break;
    }
  }
  if (name.find("qsv") != std::string::npos) {
    switch (quality) {
    case Quality_High:
      if ((ret = av_opt_set(priv_data, "preset", "veryslow", 0)) < 0) {
        LOG_ERROR(std::string("qsv set opt preset veryslow failed, ret = ") +
                  av_err2str(ret));
        return false;
      }
      break;
    case Quality_Medium:
      if ((ret = av_opt_set(priv_data, "preset", "medium", 0)) < 0) {
        LOG_ERROR(std::string("qsv set opt preset medium failed, ret = ") + av_err2str(ret));
        return false;
      }
      break;
    case Quality_Low:
      if ((ret = av_opt_set(priv_data, "preset", "veryfast", 0)) < 0) {
        LOG_ERROR(std::string("qsv set opt preset veryfast failed, ret = ") +
                  av_err2str(ret));
        return false;
      }
      break;
    default:
      break;
    }
  }
  if (name.find("mediacodec") != std::string::npos) {
    if (name.find("h264") != std::string::npos) {
      if ((ret = av_opt_set(priv_data, "level", "5.1", 0)) < 0) {
        LOG_ERROR(std::string("mediacodec set opt level 5.1 failed, ret = ") +
                  av_err2str(ret));
        return false;
      }
    }
    if (name.find("hevc") != std::string::npos) {
      // https:en.wikipedia.org/wiki/High_Efficiency_Video_Coding_tiers_and_levels
      if ((ret = av_opt_set(priv_data, "level", "h5.1", 0)) < 0) {
        LOG_ERROR(std::string("mediacodec set opt level h5.1 failed, ret = ") +
                  av_err2str(ret));
        return false;
      }
    }
  }
  return true;
}

struct CodecOptions {
  std::string codec_name;
  std::string option_name;
  std::map<int, std::string> rc_values;
};

bool set_rate_control(AVCodecContext *c, const std::string &name, int rc,
                      int q) {
  if (name.find("qsv") != std::string::npos) {
    // https://github.com/LizardByte/Sunshine/blob/3e47cd3cc8fd37a7a88be82444ff4f3c0022856b/src/video.cpp#L1635
    c->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
  }
  // constant QP ("CQ") support: nvenc uses rc=constqp + qp, amf uses rc=cqp + qp_i/qp_p/qp_b,
  // mediacodec uses bitrate_mode=cq + global_quality, qsv uses ICQ (global_quality).
  std::vector<CodecOptions> codecs = {
      {"nvenc", "rc", {{RC_CBR, "cbr"}, {RC_VBR, "vbr"}, {RC_CQ, "constqp"}}},
      {"amf",
       "rc",
       {{RC_CBR, "cbr"}, {RC_VBR, "vbr_latency"}, {RC_CQ, "cqp"}}},
      {"mediacodec",
       "bitrate_mode",
       {{RC_CBR, "cbr"}, {RC_VBR, "vbr"}, {RC_CQ, "cq"}}},
      // {"videotoolbox", "constant_bit_rate", {{RC_CBR, "1"}}},
  };
  bool has_qp = q >= 0 && q <= 51;

  for (const auto &codec : codecs) {
    if (name.find(codec.codec_name) != std::string::npos) {
      auto it = codec.rc_values.find(rc);
      if (it != codec.rc_values.end()) {
        int ret = av_opt_set(c->priv_data, codec.option_name.c_str(),
                             it->second.c_str(), 0);
        if (ret < 0) {
          LOG_ERROR(codec.codec_name + " set opt " + codec.option_name + " " +
                    it->second + " failed, ret = " + av_err2str(ret));
          return false;
        }
        if (rc == RC_CQ) {
          if (!has_qp) {
            LOG_INFO(codec.codec_name +
                     " rc=CQ but q is out of range [0, 51], keep the encoder "
                     "default QP, q = " +
                     std::to_string(q));
          } else if (name.find("mediacodec") != std::string::npos) {
            c->global_quality = q;
          } else if (name.find("nvenc") != std::string::npos) {
            if ((ret = av_opt_set_int(c->priv_data, "qp", q, 0)) < 0) {
              LOG_ERROR(std::string("nvenc set opt qp failed, ret = ") +
                        av_err2str(ret));
              return false;
            }
          } else if (name.find("amf") != std::string::npos) {
            const char *qp_opts[] = {"qp_i", "qp_p", "qp_b"};
            for (const auto *opt : qp_opts) {
              if ((ret = av_opt_set_int(c->priv_data, opt, q, 0)) < 0) {
                LOG_ERROR(std::string("amf set opt ") + opt +
                          " failed, ret = " + av_err2str(ret));
                return false;
              }
            }
          }
        }
      }
      break;
    }
  }

  // qsv has no "rc" AVOption: ffmpeg picks the rate control mode from the
  // AVCodecContext fields (see qsvenc.c select_rc_mode), so handle it here.
  if (name.find("qsv") != std::string::npos) {
    if (rc == RC_CBR) {
      // set_av_codec_ctx() sets rc_max_rate = bit_rate and then decrements bit_rate to
      // make ffmpeg choose the VBR branch; making both equal selects real CBR.
      if (c->rc_max_rate > 0) {
        c->bit_rate = c->rc_max_rate;
      }
    } else if (rc == RC_CQ) {
      // ICQ requires global_quality > 0 (see qsvenc.c select_rc_mode) and no bitrate limit.
      if (q > 0 && q <= 51) {
        c->rc_max_rate = 0;
        c->bit_rate = 0;
        c->global_quality = q;
      } else {
        LOG_INFO(std::string("qsv rc=CQ but q is out of range [1, 51], keep the "
                             "default rate control, q = ") +
                 std::to_string(q));
      }
    }
    // qsv 的码控模式由 AVCodecContext 字段决定 (qsvenc.c select_rc_mode), 没有 "rc" 选项,
    // 因此这里回读一次, 把 ffmpeg 真正会选中的模式打出来, 避免出现
    // "设了 CBR/CQ 但实际走的是 VBR" 这类静默失效。
    std::string mode;
    if (c->global_quality > 0 && c->rc_max_rate == 0) {
      mode = "ICQ(global_quality=" + std::to_string(c->global_quality) + ")";
    } else if (c->bit_rate > 0 && c->bit_rate == c->rc_max_rate) {
      mode = "CBR(bit_rate=" + std::to_string(c->bit_rate) + ")";
    } else if (c->bit_rate > 0 || c->rc_max_rate > 0) {
      mode = "VBR(target=" + std::to_string(c->bit_rate) +
             ", max=" + std::to_string(c->rc_max_rate) + ")";
    } else {
      mode = "CQP(no bitrate/quality set, ffmpeg default QP)";
    }
    const bool mismatch =
        (rc == RC_CBR && mode.compare(0, 3, "CBR") != 0) ||
        (rc == RC_CQ && mode.compare(0, 3, "ICQ") != 0);
    if (mismatch) {
      LOG_WARN("qsv rate control mismatch: requested rc=" + std::to_string(rc) +
               " but ffmpeg will use " + mode + ", name: " + name);
    } else {
      LOG_INFO("qsv rate control: " + mode + ", name: " + name);
    }
  }

  return true;
}
bool set_encode_enhance(void *priv_data, const std::string &name, int spatial_aq,
                        int temporal_aq, int multipass, int preanalysis) {
  // 画质增强项; 失败只记日志不返回失败, 避免因为一个可选项让整条会话建不起来
  int ret;
  bool applied = false;

  if (name.find("nvenc") != std::string::npos) {
    if (spatial_aq > 0) {
      if ((ret = av_opt_set_int(priv_data, "spatial-aq", 1, 0)) < 0) {
        LOG_ERROR(std::string("nvenc set opt spatial-aq failed, ret = ") +
                  av_err2str(ret));
      } else {
        applied = true;
      }
    }
    if (temporal_aq > 0) {
      // 注意: 部分 GPU 不支持 temporal AQ, ffmpeg 的能力检查会返回 ENOSYS,
      // 由调用方在 avcodec_open2 失败时去掉增强项重试
      if ((ret = av_opt_set_int(priv_data, "temporal-aq", 1, 0)) < 0) {
        LOG_ERROR(std::string("nvenc set opt temporal-aq failed, ret = ") +
                  av_err2str(ret));
      } else {
        applied = true;
      }
    }
    // 1 = two pass, quarter resolution; 2 = two pass, full resolution
    if (multipass == 1 || multipass == 2) {
      const char *v = multipass == 2 ? "fullres" : "qres";
      if ((ret = av_opt_set(priv_data, "multipass", v, 0)) < 0) {
        LOG_ERROR(std::string("nvenc set opt multipass ") + v +
                  " failed, ret = " + av_err2str(ret));
      } else {
        applied = true;
      }
    }
  } else if (name.find("amf") != std::string::npos) {
    if (preanalysis > 0) {
      if ((ret = av_opt_set_int(priv_data, "preanalysis", 1, 0)) < 0) {
        LOG_ERROR(std::string("amf set opt preanalysis failed, ret = ") +
                  av_err2str(ret));
      } else {
        applied = true;
      }
    }
  }

  const bool requested = spatial_aq > 0 || temporal_aq > 0 ||
                         multipass == 1 || multipass == 2 || preanalysis > 0;
  if (requested && !applied) {
    // 典型场景: Intel QSV / videotoolbox / vaapi 上开了 AQ/multipass。
    // 这些增强项只有 nvenc (spatial/temporal aq, multipass) 与 amf (preanalysis) 支持,
    // 其余编码器上会静默无效 —— 明确告警, 避免误以为参数已生效。
    LOG_WARN("encode enhance ignored: " + name +
             " supports none of the requested options (spatial_aq/temporal_aq/"
             "multipass are nvenc only, preanalysis is amf only), requested "
             "spatial_aq=" +
             std::to_string(spatial_aq) + ", temporal_aq=" +
             std::to_string(temporal_aq) + ", multipass=" +
             std::to_string(multipass) + ", preanalysis=" +
             std::to_string(preanalysis));
  }
  if (applied) {
    LOG_INFO("encode enhance: name=" + name + ", spatial_aq=" +
             std::to_string(spatial_aq) + ", temporal_aq=" +
             std::to_string(temporal_aq) + ", multipass=" +
             std::to_string(multipass) + ", preanalysis=" +
             std::to_string(preanalysis));
  }
  return applied;
}
bool set_gpu(void *priv_data, const std::string &name, int gpu) {
  int ret;
  if (gpu < 0)
    return -1;
  if (name.find("nvenc") != std::string::npos) {
    if ((ret = av_opt_set_int(priv_data, "gpu", gpu, 0)) < 0) {
      LOG_ERROR(std::string("nvenc set gpu failed, ret = ") + av_err2str(ret));
      return false;
    }
  }
  return true;
}

bool force_hw(void *priv_data, const std::string &name) {
  int ret;
  if (name.find("_mf") != std::string::npos) {
    if ((ret = av_opt_set_int(priv_data, "hw_encoding", 1, 0)) < 0) {
      LOG_ERROR(std::string("mediafoundation set hw_encoding failed, ret = ") +
                av_err2str(ret));
      return false;
    }
  }
  if (name.find("videotoolbox") != std::string::npos) {
    if ((ret = av_opt_set_int(priv_data, "allow_sw", 0, 0)) < 0) {
      LOG_ERROR(std::string("mediafoundation set allow_sw failed, ret = ") +
                av_err2str(ret));
      return false;
    }
  }
  return true;
}

bool set_others(void *priv_data, const std::string &name) {
  int ret;
  if (name.find("_mf") != std::string::npos) {
    // ff_eAVScenarioInfo_DisplayRemoting = 1
    if ((ret = av_opt_set_int(priv_data, "scenario", 1, 0)) < 0) {
      LOG_ERROR(std::string("mediafoundation set scenario failed, ret = ") +
                av_err2str(ret));
      return false;
    }
  }
  if (name.find("vaapi") != std::string::npos) {
    if ((ret = av_opt_set_int(priv_data, "idr_interval",
                              std::numeric_limits<int>::max(), 0)) < 0) {
      LOG_ERROR(std::string("vaapi set idr_interval failed, ret = ") + av_err2str(ret));
      return false;
    }
  }
  return true;
}

bool change_bit_rate(AVCodecContext *c, const std::string &name, int kbs) {
  if (kbs > 0) {
    c->bit_rate = kbs * 1000;
    if (name.find("qsv") != std::string::npos) {
      c->rc_max_rate = c->bit_rate;
    }
  }
  return true;
}

void vram_encode_test_callback(const uint8_t *data, int32_t len, int32_t key, const void *obj, int64_t pts) {
  (void)data;
  (void)len;
  (void)pts;
  if (obj) {
    int32_t *pkey = (int32_t *)obj;
    *pkey = key;
  }
}

} // namespace util_encode

namespace util_decode {

static bool g_flag_could_not_find_ref_with_poc = false;

bool has_flag_could_not_find_ref_with_poc() {
  bool v = g_flag_could_not_find_ref_with_poc;
  g_flag_could_not_find_ref_with_poc = false;
  return v;
}

} // namespace util_decode

extern "C" void hwcodec_set_flag_could_not_find_ref_with_poc() {
  util_decode::g_flag_could_not_find_ref_with_poc = true;
}