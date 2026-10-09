#ifndef ANNEXB_H
#define ANNEXB_H

#include <stdint.h>

#include <vector>

#include "common.h"

namespace util_decode {

// 起始码长度 (3 或 4 字节), 0 表示当前位置不是起始码。
inline int start_code_len(const uint8_t *data, int len, int pos) {
  if (pos + 3 <= len && data[pos] == 0 && data[pos + 1] == 0 &&
      data[pos + 2] == 1) {
    return 3;
  }
  if (pos + 4 <= len && data[pos] == 0 && data[pos + 1] == 0 &&
      data[pos + 2] == 0 && data[pos + 3] == 1) {
    return 4;
  }
  return 0;
}

// 从 Annex-B 码流里摘出参数集 (H264: SPS/PPS; H265: VPS/SPS/PPS), 统一用 4 字节
// 起始码拼进 out, 供 ffmpeg 当 extradata 解析。
//
// 为什么需要: ffmpeg 的 mediacodec wrapper 在 open 阶段会无条件解析 extradata
// (libavcodec/mediacodecdec.c: h264_set_extradata -> ff_h264_decode_extradata),
// 传 NULL/0 直接返回 AVERROR(EINVAL), 于是 avcodec_open2 报 "Invalid argument"。
// 而 RustDesk 送的是裸 Annex-B (SPS/PPS 内联在码流里, 协议不带外 csd), 所以只能
// 自己先摘出来喂给它。
//
// 每个类型最多收一次: 既让 out 有界 (不会因重复包无限增长), 也让调用方可以
// 跨包累积 —— 参数集被拆到多个包里时, 后续包补齐即可。
// got_* 为 in/out 参数, 记录累计已收到的类型; 返回是否已凑齐当前格式所需的全部。
inline bool collect_parameter_sets(const uint8_t *data, int len, DataFormat format,
                                   std::vector<uint8_t> &out, bool &got_vps,
                                   bool &got_sps, bool &got_pps) {
  const bool is_h264 = format == DataFormat::H264;
  if (data && len > 0) {
    int pos = 0;
    while (pos + 3 <= len) {
      int sc = start_code_len(data, len, pos);
      if (sc == 0) {
        pos++;
        continue;
      }
      const int nal_start = pos + sc;
      int nal_end = len;
      for (int i = nal_start; i + 3 <= len; i++) {
        if (start_code_len(data, len, i) > 0) {
          nal_end = i;
          break;
        }
      }
      if (nal_start < nal_end) {
        const int type =
            is_h264 ? (data[nal_start] & 0x1f) : ((data[nal_start] >> 1) & 0x3f);
        bool want = false;
        if (is_h264) {
          want = (type == 7 && !got_sps) || (type == 8 && !got_pps);
        } else {
          want = (type == 32 && !got_vps) || (type == 33 && !got_sps) ||
                 (type == 34 && !got_pps);
        }
        if (want) {
          static const uint8_t k_start_code[4] = {0, 0, 0, 1};
          out.insert(out.end(), k_start_code, k_start_code + 4);
          out.insert(out.end(), data + nal_start, data + nal_end);
          if (type == 7 || type == 33) {
            got_sps = true;
          } else if (type == 8 || type == 34) {
            got_pps = true;
          } else {
            got_vps = true;
          }
        }
      }
      pos = nal_end;
    }
  }
  return is_h264 ? (got_sps && got_pps) : (got_vps && got_sps && got_pps);
}

} // namespace util_decode

#endif // ANNEXB_H
