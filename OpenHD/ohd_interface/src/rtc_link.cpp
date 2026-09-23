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

#include "rtc_link.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "Publisher.h"
#include "Subscriber.h"
#include "openhd_spdlog.h"
#include "openhd_spdlog_include.h"

namespace {

// ground 转发打包 MTU：与 WFB 路径一致（设计 §9）
constexpr int kGroundRtpMtu = 1440;
// 订阅看护 tick 与最小重建间隔（设计 §8/§9）
constexpr uint64_t kSupervisorSleepStepMs = 100;
constexpr int kSupervisorStepsPerTick = 10;
constexpr uint64_t kSubscribeRetryMinGapMs = 2000;
// SFU 帧类型（Subscriber OnFrame payload 首字节，设计 F6）
constexpr uint8_t kFrameTypeParams = 0x01;
constexpr uint8_t kFrameTypeIdr = 0x02;
constexpr uint8_t kFrameTypeP = 0x03;
constexpr uint8_t kFrameTypeAudio = 0x10;

uint64_t now_us() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

uint64_t now_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

RtcLink::RtcLink(OHDProfile profile)
    : m_config(RtcConfig::load()), m_profile(std::move(profile)) {
  m_console = openhd::log::create_or_get("rtc_link");
  if (!m_config.is_valid()) {
    m_console->error("RtcLink 未启动：rtc.txt 缺少 server_ip");
    return;
  }
  if (m_profile.is_air) {
    init_air_unit();
  } else {
    init_ground_unit();
  }
}

RtcLink::~RtcLink() {
  m_stopping.store(true);
  if (m_supervisor_thread.joinable()) {
    m_supervisor_thread.join();
  }
  for (auto& s : m_streams) {
    if (s.publisher) s.publisher->Stop();
    if (s.subscriber) s.subscriber->Stop();
  }
}

std::string RtcLink::stream_id(int stream_index) const {
  return m_config.stream_prefix + std::to_string(stream_index);
}

void RtcLink::init_air_unit() {
  for (int i = 0; i < 2; ++i) {
    auto& s = m_streams[i];
    s.index = i;
    s.enabled = m_config.enable_cam[i];
    if (!s.enabled) {
      m_console->warn("cam{} 未启用 RTC 链路（enable_cam{}=0）", i, i);
      continue;
    }
    // RTP 分片 → 完整 Annex-B AU（成帧依据：ts 变化/marker，500ms 超时兜底）
    s.assembler = std::make_unique<RtpEsAssembler>(
        false,
        [this, i](std::vector<uint8_t>&& au, bool is_param, bool is_key) {
          auto& st = m_streams[i];
          if (st.publisher) {
            st.publisher->PushVideoFrame(au.data(), au.size(), is_param,
                                         is_key);
          }
        });
    s.assembler->set_on_warn([this, i](const std::string& msg) {
      m_console->warn("cam{} [RTP→AU] {}", i, msg);
    });
    s.publisher = std::make_unique<push::Publisher>();
    s.publisher->SetBandwidth(m_config.pacer_mbps * 1000ULL * 1000ULL);
    s.publisher->SetOnConnected([this, i](uint32_t session_id) {
      m_console->warn("cam{} 已接入 SFU (session {})，开始推流", i, session_id);
    });
    s.publisher->SetOnDisconnected([this, i]() {
      m_console->warn("cam{} 与 SFU 断连，SDK 自动重连中", i);
    });
    // 回调处于 SDK 发送锁内，仅计数不阻塞；日志在 gst 线程侧延迟输出
    s.publisher->SetOnKeyFrameRequest([this, i]() {
      m_streams[i].pli_count.fetch_add(1, std::memory_order_relaxed);
    });
    s.publisher->SetOnControlData(
        [this, i](uint32_t /*source_session*/, const uint8_t* data,
                  size_t len) { on_control_data(i, data, len); });
    const bool started =
        s.publisher->Start(m_config.server_ip, m_config.tcp_port, stream_id(i),
                           m_config.user_id_air, m_config.use_udp,
                           m_config.effective_udp_port());
    m_console->warn("推流 cam{} stream={} → {}:{} ({}) {}", i, stream_id(i),
                    m_config.server_ip, m_config.tcp_port,
                    m_config.use_udp ? "UDP" : "TCP",
                    started ? "已启动" : "启动失败");
  }
  m_console->warn(
      "RtcLink air 就绪：编码 RTP → 完整 AU → SFU（H.264；PLI v1 仅记录）");
}

void RtcLink::init_ground_unit() {
  for (int i = 0; i < 2; ++i) {
    auto& s = m_streams[i];
    s.index = i;
    s.enabled = m_config.enable_cam[i];
    if (!s.enabled) {
      m_console->warn("cam{} 未启用 RTC 链路（enable_cam{}=0）", i, i);
      continue;
    }
    // 完整 AU → RTP 包（FU-A 分片），逐包走 OHDLink 出口（QOpenHD 无感知）
    s.packetizer = std::make_unique<EsRtpPacketizer>(
        false, kGroundRtpMtu, [this, i](const uint8_t* rtp, size_t len) {
          on_receive_video_data(i, rtp, static_cast<int>(len));
          m_streams[i].rtp_out.fetch_add(1, std::memory_order_relaxed);
        });
    start_ground_subscriber(s);
  }
  m_supervisor_thread = std::thread([this]() { supervisor_loop(); });
  m_console->warn(
      "RtcLink ground 就绪：SFU → 完整 AU → RTP（转发 5600/5601 等由 "
      "ohd_video 既有路径完成）");
}

void RtcLink::start_ground_subscriber(StreamState& s) {
  const int i = s.index;
  s.disconnected.store(false);
  auto sub = std::make_unique<pull::Subscriber>();
  sub->SetOnConnected([this, i](uint32_t session_id) {
    m_console->warn("cam{} 已订阅 SFU 流 (session {})", i, session_id);
    // 首帧快速出画（SFU 对丢包/新订阅也会自行触发 PLI，重复无害）
    m_streams[i].subscriber->RequestKeyFrame();
  });
  sub->SetOnDisconnected([this, i]() {
    m_console->warn("cam{} 订阅断链，等待看护重建", i);
    m_streams[i].disconnected.store(true);
  });
  sub->SetOnSubscribeFailed([this, i](const std::string& reason) {
    m_console->warn("cam{} 订阅失败: {}（air 可能尚未上线，将自动重试）", i,
                    reason);
    m_streams[i].disconnected.store(true);
  });
  sub->SetOnFrame([this, i](uint32_t /*source_session*/,
                            std::vector<uint8_t>&& data) {
    if (data.empty()) return;
    auto& st = m_streams[i];
    const uint8_t frame_type = data[0];
    const uint8_t* payload = data.data() + 1;
    const size_t payload_len = data.size() - 1;
    if (frame_type == kFrameTypeParams || frame_type == kFrameTypeIdr ||
        frame_type == kFrameTypeP) {
      st.au_in.fetch_add(1, std::memory_order_relaxed);
      if (st.packetizer) {
        st.packetizer->on_au(frame_type, payload, payload_len);
      }
    } else if (frame_type == kFrameTypeAudio) {
      st.audio_frames.fetch_add(1, std::memory_order_relaxed);
      // PCMA RTP 包不透明透传：5610 转发由 ohd_video 既有路径完成（设计 §4.4）
      on_receive_audio_data(payload, static_cast<int>(payload_len));
    }
    // 其余类型（通知等）静默忽略
  });
  sub->SetOnControlData(
      [this, i](uint32_t /*source_session*/, const uint8_t* data, size_t len) {
        on_control_data(i, data, len);
      });
  s.subscriber = std::move(sub);
  // 非阻塞：连接/订阅在 SDK 网络线程内完成，失败经 OnSubscribeFailed 通知
  s.subscriber->Start(m_config.server_ip, m_config.tcp_port, stream_id(i),
                      m_config.user_id_ground, m_config.use_udp,
                      m_config.effective_udp_port());
  m_last_sub_attempt_ms.store(now_ms());
}

void RtcLink::supervisor_loop() {
  int tick = 0;
  while (!m_stopping.load(std::memory_order_relaxed)) {
    for (int j = 0; j < kSupervisorStepsPerTick; ++j) {
      if (m_stopping.load(std::memory_order_relaxed)) return;
      std::this_thread::sleep_for(
          std::chrono::milliseconds(kSupervisorSleepStepMs));
    }
    ++tick;
    for (auto& s : m_streams) {
      if (!s.enabled || !s.subscriber) continue;
      // 失败路径：订阅被拒（SDK 已置 m_running=false）；断连路径：线程退出 +
      // OnDisconnected 标志（SDK 不断言 m_running，F10）
      const bool net_thread_dead = !s.subscriber->IsRunning();
      if (!s.disconnected.load() && !net_thread_dead) continue;
      if (now_ms() - m_last_sub_attempt_ms.load() < kSubscribeRetryMinGapMs) {
        continue;
      }
      m_console->warn("cam{} 重建订阅（{}）", s.index,
                      net_thread_dead ? "网络线程已退出" : "连接断开");
      s.subscriber->Stop();
      start_ground_subscriber(s);
    }
    if (tick % 10 == 0) log_ground_stats();
  }
}

void RtcLink::log_ground_stats() {
  for (auto& s : m_streams) {
    if (!s.enabled || !s.subscriber) continue;
    const auto stats = s.subscriber->GetStats();
    m_console->debug(
        "cam{}: au_in={} rtp_out={} audio={} | fec: frames={} blocks_lost={} "
        "rtt={}ms loss={:.3f}",
        s.index, s.au_in.load(std::memory_order_relaxed),
        s.rtp_out.load(std::memory_order_relaxed),
        s.audio_frames.load(std::memory_order_relaxed), stats.frames_completed,
        stats.blocks_lost, stats.rtt_ms, stats.loss_rate);
  }
  if (m_ctrl_rx.load(std::memory_order_relaxed) != 0 ||
      m_ctrl_tx.load(std::memory_order_relaxed) != 0) {
    m_console->debug("ctrl: rx(下行遥测)={} tx(上行 RC/遥测)={}",
                     m_ctrl_rx.load(std::memory_order_relaxed),
                     m_ctrl_tx.load(std::memory_order_relaxed));
  }
}

void RtcLink::transmit_video_data(
    int stream_index,
    const openhd::FragmentedVideoFrame& fragmented_video_frame) {
  if (stream_index < 0 || stream_index > 1) return;
  auto& s = m_streams[stream_index];
  if (!s.enabled || s.publisher == nullptr || s.assembler == nullptr) {
    return;  // ground 角色或该相机未启用
  }
  if (fragmented_video_frame.enable_ultra_secure_encryption) {
    // tj_rtc 无载荷加密能力（设计 R4）：告警并丢弃，绝不静默降级为明文
    if (!m_warned_encryption.exchange(true)) {
      m_console->error(
          "帧要求加密传输，但 tj_rtc 不支持加密：该路视频将被丢弃（不会明文发送）");
    }
    return;
  }
  if (s.assembler->video_stopped()) {
    if (!s.stop_logged) {
      s.stop_logged = true;
      m_console->error("cam{} 疑似非 H.264 码流，已停止推流该路视频（遥测保留）",
                       stream_index);
    }
    return;
  }
  for (const auto& fragment : fragmented_video_frame.rtp_fragments) {
    s.assembler->on_rtp_packet(fragment->data(), fragment->size());
  }
  // 500ms 超时兜底（正常帧由 ts 变化/marker 收尾）
  s.assembler->flush(now_us());
  const auto pli = s.pli_count.load(std::memory_order_relaxed);
  if (pli != s.pli_logged && (pli == 1 || pli - s.pli_logged >= 50)) {
    s.pli_logged = pli;
    m_console->warn(
        "cam{} 收到关键帧请求(PLI) 累计 {}：v1 不强制 IDR，依赖周期关键帧",
        stream_index, pli);
  }
}

int RtcLink::ctrl_stream_index() const {
  // 遥测/RC/音频共用一条 ctrl 通道，固定挂在首个启用的相机流上
  // （air/ground 两侧 rtc.txt 的 enable_cam 需保持一致）
  return m_config.enable_cam[0] ? 0 : 1;
}

void RtcLink::transmit_telemetry_data(TelemetryTxPacket packet) {
  if (!packet.data || packet.data->empty()) return;
  auto& s = m_streams[ctrl_stream_index()];
  if (!s.enabled) return;
  const bool is_rc = packet.packet_type == TelemetryPacketType::RC;
  // §4.3：默认 unreliable（UDP 按 n_injections 复制 1/2/4 次）；可靠模式 TCP 单发
  const bool reliable = is_rc ? m_config.rc_reliable : m_config.telemetry_reliable;
  const int n = reliable ? 1 : std::max(1, packet.n_injections);
  const uint8_t* data = packet.data->data();
  const size_t len = packet.data->size();
  if (s.publisher) {  // air：空→地 遥测/RC（SFU 广播给该流全部订阅者）
    for (int i = 0; i < n; ++i) {
      s.publisher->SendCtrlDownstream(data, len, reliable);
    }
  } else if (s.subscriber) {  // ground：地→空（P1 使上游窗口不依赖首个媒体帧）
    for (int i = 0; i < n; ++i) {
      s.subscriber->SendCtrlUpstream(data, len, reliable);
    }
  } else {
    return;
  }
  const auto c = m_ctrl_tx.fetch_add(1, std::memory_order_relaxed) + 1;
  if (c == 1 || c % 200 == 0) {
    m_console->warn("{} ctrl 已发送 {} 包（每包 {}B，{}，injections={}）",
                    is_rc ? "RC" : "遥测", c, len,
                    reliable ? "TCP 可靠单发" : "UDP", n);
  }
}

void RtcLink::transmit_audio_data(const openhd::AudioPacket& audio_packet) {
  if (!audio_packet.data || audio_packet.data->empty()) return;
  auto& s = m_streams[ctrl_stream_index()];
  if (!s.enabled || !s.publisher) return;  // ground 角色不发送音频
  // PCMA RTP 包不透明透传（设计 §4.4）：不做 PCM 转换
  s.publisher->PushAudioFrame(audio_packet.data->data(),
                              audio_packet.data->size());
  const auto c = m_audio_tx.fetch_add(1, std::memory_order_relaxed) + 1;
  if (c == 1) {
    m_console->warn("音频通道已接入（PushAudioFrame，首包 {} 字节）",
                    audio_packet.data->size());
  }
}

void RtcLink::on_control_data(int stream_index, const uint8_t* data,
                              size_t len) {
  (void)stream_index;
  // air：地→空 遥测/RC；ground：空→地。与 WFB 接收线程调用路径等价
  auto shared = std::make_shared<std::vector<uint8_t>>(data, data + len);
  const auto n = m_ctrl_rx.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || n % 200 == 0) {
    m_console->warn("遥测/RC ctrl 已收到 {} 包（最近 {} 字节）", n, len);
  }
  on_receive_telemetry_data(std::move(shared));
}