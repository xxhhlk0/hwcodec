#ifndef UTIL_H
#define UTIL_H

#include <map>
#include <string>
#include <chrono>
#include <thread>
extern "C" {
#include <libavcodec/avcodec.h>
}

namespace util_encode {

// 编码参数透传: 形如 "key=value;key=value" 的字符串拆成表, 各厂商只读取自己
// 认识的 key, 未出现的 key 一律沿用 SDK 默认值 (即不传时行为不变)。
std::map<std::string, std::string> parse_opts(const char *opts);
bool has_opt(const std::map<std::string, std::string> &opts, const char *key);
int opt_int(const std::map<std::string, std::string> &opts, const char *key,
            int fallback);
bool opt_flag(const std::map<std::string, std::string> &opts, const char *key,
              bool fallback);

void set_av_codec_ctx(AVCodecContext *c, const std::string &name, int kbs,
                      int gop, int fps);
bool set_lantency_free(void *priv_data, const std::string &name);
// qsv/vaapi 硬件编码的吞吐相关选项 (上游写死的最低延迟值会腰斩吞吐),
// 实测数据与覆盖用环境变量见 util.cpp 内注释。
int hw_async_depth();
// 硬件帧池 surface 数量 (默认 4): 池 = 1 时每帧写入必须等上一帧编码完, 流水线失效。
int hw_pool_size();
// qsv: low_power=1 + low_delay_brc=1; 返回是否下发了至少一项 (调用方据此在
// avcodec_open2 失败时回退重试)。非 qsv 编码器直接返回 false。
bool apply_qsv_low_latency(void *priv_data, const std::string &name);
bool revert_qsv_low_latency(void *priv_data, const std::string &name);
bool set_quality(void *priv_data, const std::string &name, int quality);
bool set_rate_control(AVCodecContext *c, const std::string &name, int rc,
                      int q);
// 画质增强 (可选, 均为编码器内建能力): nvenc spatial-aq/temporal-aq/multipass,
// amf preanalysis。返回是否应用了至少一项 (调用方据此在初始化失败时回退重试)。
bool set_encode_enhance(void *priv_data, const std::string &name, int spatial_aq,
                        int temporal_aq, int multipass, int preanalysis);
bool set_gpu(void *priv_data, const std::string &name, int gpu);
bool force_hw(void *priv_data, const std::string &name);
bool set_others(void *priv_data, const std::string &name);

bool change_bit_rate(AVCodecContext *c, const std::string &name, int kbs);
void vram_encode_test_callback(const uint8_t *data, int32_t len, int32_t key, const void *obj, int64_t pts);

} // namespace util

namespace util_decode {
    bool has_flag_could_not_find_ref_with_poc();
}

namespace util {

    inline std::chrono::steady_clock::time_point now() {
        return std::chrono::steady_clock::now();
    }

    inline int64_t elapsed_ms(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now() - start).count();
    }

    // 编码器 (async_depth > 1 的 QSV/VAAPI) 在管线未填满时可能暂时拿不到包。
    // 这种情况应短暂等待, 而不是立即返回失败: 调用方会把"没包"当成编码失败,
    // 首帧失败时甚至会直接切掉硬件编码器。
    inline void sleep_ms(int64_t ms) {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    inline bool skip_test(const int64_t *excludedLuids, const int32_t *excludeFormats, int32_t excludeCount, int64_t currentLuid, int32_t dataFormat) {
      for (int32_t i = 0; i < excludeCount; i++) {
        if (excludedLuids[i] == currentLuid && excludeFormats[i] == dataFormat) {
          return true;
        }
      }
      return false;
    }
}


#endif
