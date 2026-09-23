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

#ifndef OPENHD_ES_RTP_PACKETIZER_H
#define OPENHD_ES_RTP_PACKETIZER_H

#include <cstdint>
#include <functional>
#include <vector>

/**
 * ground 方向（RtcLink 视频出口）：把 tj_rtc Subscriber 收到的 Annex-B 完整
 * 访问单元（[frameType][Annex-B]，frameType 0x01=参数集/0x02=IDR/0x03=P）重新
 * 打包成 RTP 包，逐包经 OHDLink::on_receive_video_data 转发（等价 WFB 行为）。
 *
 * 设计要点（详见 openhd_rtc_link_design.md §4.2）：
 * - NALU ≤ mtu-12 单包；否则 FU-A 分片（每片 body ≤ mtu-14），末片带 marker
 * - RTP：V=2、P/X/CC=0、PT=96、SSRC=0x4F504844、seq 每流递增、ts=steady 时钟 90kHz
 * - 仅打包 NALU 类型 1..5/7/8（防御性过滤 SEI/AUD 等）
 * - 依赖仅 std：不 include openhd::log / OHDLink，保证可离线独立编译与自测（L0）
 */
class EsRtpPacketizer {
 public:
  // SendCb 必须同步完成发送（内部复用 scratch 缓冲，回调返回后内容即失效）
  using SendCb = std::function<void(const uint8_t* rtp, size_t len)>;

  EsRtpPacketizer(bool is_h265, int mtu, SendCb send_cb);

  // frame_type: 0x01 参数集 / 0x02 IDR / 0x03 P（其余忽略）；
  // data/len: Annex-B 访问单元（可含多个 NALU，3/4 字节起始码）
  void on_au(uint8_t frame_type, const uint8_t* data, size_t len);

 private:
  void begin_packet(bool marker);
  void finish_packet();
  void send_single_nalu(const uint8_t* nalu, size_t len, bool marker);
  void send_fu_a(const uint8_t* nalu, size_t len, bool marker);
  static uint32_t next_ts_90k();

  static constexpr uint8_t kRtpPayloadType = 96;
  static constexpr uint32_t kSsrc = 0x4F504844;  // "OPHD"
  static constexpr size_t kRtpHeaderSize = 12;

  const bool m_is_h265;  // v1 仅支持 H.264，保留标记（对应设计 §4.1 弱校验）
  int m_mtu;
  SendCb m_send_cb;

  uint16_t m_seq = 0;
  uint32_t m_cur_ts = 0;
  std::vector<uint8_t> m_scratch;
};

#endif  // OPENHD_ES_RTP_PACKETIZER_H