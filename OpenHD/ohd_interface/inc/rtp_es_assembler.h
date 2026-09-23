/******************************************************************************
 * OpenHD
 *
 * Licensed under the GNU General Public License (GPL) Version 3.
 *
 * This software is provided "as-is," without warranty of any kind, express or
 * implied, including but not limited to the warranties of merchantability,
 * fitness for a particular purpose, and non-infringement. For details, see the
 * full license in the LICENSE file provided with this source code.
 *
 * Non-Military Use Only:
 * This software and its associated components are explicitly intended for
 * civilian and non-military purposes. Use in any military or defense
 * applications is strictly prohibited unless explicitly and individually
 * licensed otherwise by the OpenHD Team.
 *
 * Contributors:
 * A full list of contributors can be found at the OpenHD GitHub repository:
 * https://github.com/OpenHD
 *
 * © OpenHD, All Rights Reserved.
 ******************************************************************************/

#ifndef OPENHD_RTP_ES_ASSEMBLER_H
#define OPENHD_RTP_ES_ASSEMBLER_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * air 方向（RtcLink 视频入口）：把本机 gstreamer 产出的 RTP/H.264 包重组为
 * Annex-B 完整访问单元（AU），供 tj_rtc Publisher::PushVideoFrame 使用。
 *
 * 设计要点（详见 openhd_rtc_link_design.md §4.1）：
 * - 成帧依据：RTP timestamp 变化 / marker 位 / 500ms 超时，不使用 OpenHD 的松帧边界
 * - 输出约定与 NaluSplitter 一致：参数集与 VCL 同 AU 合并（[SPS][PPS][IDR] 一帧），
 *   纯参数集 AU 以 is_param=true 单独输出，SEI/AUD 丢弃
 * - 输入来自进程内 gstreamer appsink（无丢包/乱序），仅做超时兜底，不做序号缺口容错
 * - 依赖仅 std：不 include openhd::log / OHDLink，保证可离线独立编译与自测（L0）
 */
class RtpEsAssembler {
 public:
  // au: 完整 Annex-B AU（4 字节起始码拼接）；is_param: 纯参数集 AU；
  // is_key: AU 内含 IDR（type==5）
  using OutCb = std::function<void(std::vector<uint8_t>&& au, bool is_param,
                                   bool is_key)>;
  // 告警（可多次触发）：FU 失步、AU 超限、H.265 弱校验等，由使用方决定日志/上报
  using WarnCb = std::function<void(const std::string& msg)>;

  RtpEsAssembler(bool is_h265, OutCb out_cb);

  void set_on_warn(WarnCb warn_cb);

  // 处理一个完整 RTP 包（含 12B 头部）；线程亲和：单线程（gst highspeed 消费线程）
  void on_rtp_packet(const uint8_t* pkt, size_t len);

  // 由使用方定时调用：当前 AU 自开始累积超过 500ms 未收尾时强制输出（防丢包卡死）
  void flush(uint64_t now_us);

  // H.265 弱校验触发后为 true：使用方应停止推流该路视频（遥测保留）
  bool video_stopped() const { return m_video_stopped; }

 private:
  void emit_nalu(const uint8_t* nalu, size_t len);
  void finalize_au();
  void open_au_if_needed();
  void note_invalid_nalu_type();
  void reset_invalid_count() { m_consecutive_invalid = 0; }
  void warn(const std::string& msg);
  static uint64_t now_us();

  static constexpr uint64_t kAuTimeoutUs = 500 * 1000;
  static constexpr size_t kMaxAuBytes = 4 * 1024 * 1024;
  // 连续非法 NALU 类型包数达到该值 → 判定疑似 H.265/非 H.264 流（弱校验）
  static constexpr int kMaxConsecutiveInvalid = 32;

  const bool m_is_h265;
  OutCb m_out_cb;
  WarnCb m_warn_cb;

  std::vector<std::vector<uint8_t>> m_params;
  std::vector<std::vector<uint8_t>> m_vcl;
  std::vector<uint8_t> m_fu_nalu;  // 进行中的 FU-A 重组缓冲
  bool m_has_idr = false;
  bool m_fu_in_progress = false;
  bool m_au_open = false;
  bool m_video_stopped = false;
  uint32_t m_cur_ts = 0;
  uint32_t m_last_seen_ts = 0;
  uint64_t m_au_start_us = 0;
  size_t m_au_bytes = 0;
  int m_consecutive_invalid = 0;
};

#endif  // OPENHD_RTP_ES_ASSEMBLER_H