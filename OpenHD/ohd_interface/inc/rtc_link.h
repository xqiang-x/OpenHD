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

#ifndef OPENHD_RTC_LINK_H
#define OPENHD_RTC_LINK_H

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "es_rtp_packetizer.h"
#include "openhd_link.hpp"
#include "openhd_profile.h"
#include "rtc_config.h"
#include "rtp_es_assembler.h"

namespace push {
class Publisher;
}
namespace pull {
class Subscriber;
}

/**
 * RtcLink：以 tj_rtc SDK（SFU 中转，UDP+FEC/NACK/自适应码率）作为 OpenHD 与
 * WFB(RTP) 并列的传输链路。
 * - air：编码器 RTP 分片 → RtpEsAssembler（完整 Annex-B AU）→ Publisher::PushVideoFrame
 * - ground：Subscriber 收完整 AU → EsRtpPacketizer → on_receive_video_data
 *   （QOpenHD 经 5600/5601 无感知，转发出 ohd_video 既有路径完成）
 * - 遥测/RC：air=SendCtrlDownstream，ground=SendCtrlUpstream（挂在首个启用
 *   相机流的 ctrl 通道；默认 UDP 按 n_injections 复制，可靠模式走 TCP 单发）
 * - 音频：air=PushAudioFrame（PCMA RTP 不透明透传），ground=0x10→on_receive_audio_data
 * 详见 openhd_rtc_link_design.md §4.3/§4.4。
 */
class RtcLink : public OHDLink {
 public:
  explicit RtcLink(OHDProfile profile);
  ~RtcLink();

  void transmit_telemetry_data(TelemetryTxPacket packet) override;
  void transmit_video_data(
      int stream_index,
      const openhd::FragmentedVideoFrame& fragmented_video_frame) override;
  void transmit_audio_data(const openhd::AudioPacket& audio_packet) override;

 private:
  struct StreamState {
    int index = 0;
    bool enabled = false;
    // air 方向
    std::unique_ptr<push::Publisher> publisher;
    std::unique_ptr<RtpEsAssembler> assembler;
    // ground 方向
    std::unique_ptr<pull::Subscriber> subscriber;
    std::unique_ptr<EsRtpPacketizer> packetizer;
    // ground：订阅断连/失败标志，由 supervisor 重建
    std::atomic<bool> disconnected{false};
    // air：PLI 计数（SDK 回调线程写）；pli_logged 仅在 gst 线程读写
    std::atomic<uint64_t> pli_count{0};
    uint64_t pli_logged = 0;
    bool stop_logged = false;  // 仅 gst 线程
    // ground：统计（supervisor 周期日志）
    std::atomic<uint64_t> au_in{0};
    std::atomic<uint64_t> rtp_out{0};
    std::atomic<uint64_t> audio_frames{0};
  };

  void init_air_unit();
  void init_ground_unit();
  void start_ground_subscriber(StreamState& stream);
  void supervisor_loop();
  void log_ground_stats();
  void on_control_data(int stream_index, const uint8_t* data, size_t len);
  std::string stream_id(int stream_index) const;
  int ctrl_stream_index() const;

  RtcConfig m_config;
  OHDProfile m_profile;
  std::array<StreamState, 2> m_streams;
  std::shared_ptr<spdlog::logger> m_console;
  std::atomic<bool> m_stopping{false};
  std::thread m_supervisor_thread;  // 仅 ground
  std::atomic<uint64_t> m_last_sub_attempt_ms{0};
  std::atomic<uint64_t> m_ctrl_tx{0};   // 遥测/RC 发送包数
  std::atomic<uint64_t> m_ctrl_rx{0};   // 遥测/RC 接收包数
  std::atomic<uint64_t> m_audio_tx{0};  // 音频发送包数（air）
  std::atomic_bool m_warned_encryption{false};
};

#endif  // OPENHD_RTC_LINK_H