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

// L1 本机全链仿真探针（ground 侧）——OpenHD ground 视频路径的最小替身：
//   SFU --> [Subscriber] --> 帧回调(类型字节) --> [EsRtpPacketizer] --> UDP:5610 --> gst 解码
// 视频路径与 RtcLink 最终实现同源（同一 Packetizer 实例化）；逐包转发等价 WFB 行为。
// 与 gst 之间用裸 UDP 对接，不引入 gst 依赖。
// M5 起：--ctrl-hz 周期性上行 MAVLink（RC/心跳，验证 ctrl 上行 + P1 上游窗口），
//        下行 ctrl 载荷按 MAVLink 帧顺序解析计数；订阅失败自动重试（2s）。
//
// 构建（独立迷你 CMake，不进 OpenHD 主构建）：
//   cmake -S OpenHD/OpenHD/ohd_interface/test/rtc_probe -B /tmp/rtc_probe_build
//   cmake --build /tmp/rtc_probe_build -j
// 运行示例（配合 gst 收端，见 openhd_rtc_link_design.md §10.2）：
//   ./rtc_gnd_probe --server 106.15.177.248 --stream rtcprobe1
//       --out-port 5610 --dump-au /tmp/l1_gnd.h264 --seconds 20

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Subscriber.h"
#include "es_rtp_packetizer.h"

namespace {

std::atomic<bool> g_running{true};

void handle_signal(int /*sig*/) { g_running.store(false); }

// ── MAVLink v2 最小收发（L4：上行 RC/心跳 + 下行遥测解析计数） ──
// 载荷为 OpenHD 聚合的连续帧流，顺序解析即可，无需按 magic 扫描。
uint16_t mav_crc_accumulate(uint8_t b, uint16_t crc) {
  uint8_t tmp = static_cast<uint8_t>(b ^ (crc & 0xFF));
  tmp = static_cast<uint8_t>(tmp ^ (tmp << 4));
  return static_cast<uint16_t>((crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4));
}

uint16_t mav_crc(const uint8_t* p, size_t n, uint16_t crc = 0xFFFF) {
  for (size_t i = 0; i < n; ++i) crc = mav_crc_accumulate(p[i], crc);
  return crc;
}

struct MavFrameInfo {
  uint8_t sysid = 0;
  uint8_t compid = 0;
  uint32_t msgid = 0;
  const uint8_t* payload = nullptr;
  uint8_t payload_len = 0;
  size_t frame_len = 0;  // 含 2B 校验
};

// 解析一帧（v1 0xFE / v2 0xFD）；不做 CRC 校验（上游为可信 OpenHD 聚合流）
bool mav_parse(const uint8_t* p, size_t n, MavFrameInfo& fi) {
  if (n < 8) return false;
  if (p[0] == 0xFD) {
    const uint8_t len = p[1];
    const size_t frame_len = 10 + static_cast<size_t>(len) + 2;
    if (n < frame_len) return false;
    fi.sysid = p[5];
    fi.compid = p[6];
    fi.msgid = static_cast<uint32_t>(p[7]) |
               (static_cast<uint32_t>(p[8]) << 8) |
               (static_cast<uint32_t>(p[9]) << 16);
    fi.payload = p + 10;
    fi.payload_len = len;
    fi.frame_len = frame_len;
    return true;
  }
  if (p[0] == 0xFE) {
    const uint8_t len = p[1];
    const size_t frame_len = 6 + static_cast<size_t>(len) + 2;
    if (n < frame_len) return false;
    fi.sysid = p[3];
    fi.compid = p[4];
    fi.msgid = p[5];
    fi.payload = p + 6;
    fi.payload_len = len;
    fi.frame_len = frame_len;
    return true;
  }
  return false;
}

// 组装 MAVLink v2 帧；返回帧长（12 + payload_len）
size_t mav_build_v2(uint8_t* out, uint8_t sysid, uint8_t compid, uint32_t msgid,
                    const uint8_t* payload, uint8_t len, uint8_t crc_extra,
                    uint8_t seq) {
  out[0] = 0xFD;
  out[1] = len;
  out[2] = 0;  // incompat_flags
  out[3] = 0;  // compat_flags
  out[4] = seq;
  out[5] = sysid;
  out[6] = compid;
  out[7] = static_cast<uint8_t>(msgid & 0xFF);
  out[8] = static_cast<uint8_t>((msgid >> 8) & 0xFF);
  out[9] = static_cast<uint8_t>((msgid >> 16) & 0xFF);
  if (len > 0) std::memcpy(out + 10, payload, len);
  uint16_t crc = mav_crc(out + 1, 9);
  crc = mav_crc(payload, len, crc);
  crc = mav_crc_accumulate(crc_extra, crc);
  out[10 + len] = static_cast<uint8_t>(crc & 0xFF);
  out[11 + len] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  return 12 + static_cast<size_t>(len);
}

struct Options {
  std::string server = "127.0.0.1";
  int server_port = 9200;
  std::string stream_id = "rtcprobe1";
  std::string user_id = "gnd_probe";
  bool use_udp = true;
  std::string out_ip = "127.0.0.1";
  int out_port = 5610;
  int mtu = 1440;
  std::string dump_au;
  int seconds = 0;  // 0 = 无限
  bool pli_once = true;
  bool verbose = false;
  int ctrl_hz = 0;  // >0 时按此频率上行 MAVLink RC（0=不上行）
};

void print_usage(const char* prog) {
  std::fprintf(stderr,
               "Usage: %s [options]\n"
               "  --server IP       SFU 服务器 (默认 127.0.0.1)\n"
               "  --port  PORT      SFU 端口 (默认 9200)\n"
               "  --stream  ID      流名 (默认 rtcprobe1)\n"
               "  --user  ID        用户 ID (默认 gnd_probe)\n"
               "  --tcp             使用 TCP 拉流 (默认 UDP)\n"
               "  --out-ip IP       RTP 转发目标 IP (默认 127.0.0.1)\n"
               "  --out-port PORT   RTP 转发目标端口 (默认 5610)\n"
               "  --mtu N           RTP 打包 MTU (默认 1440)\n"
               "  --dump-au PATH    收到的 AU 落盘, 供与 air 端逐字节比对\n"
               "  --no-pli          不在首帧后自动请求一次关键帧\n"
               "  --ctrl-hz N       >0 时按此频率上行 MAVLink RC (msgid=70),\n"
               "                    0=不上行 (L4 用; 建议 10)\n"
               "  --seconds N       运行秒数, 0=无限 (默认 0)\n"
               "  --verbose         打印每帧与每秒统计\n"
               "  -h, --help        显示本帮助\n",
               prog);
}

bool parse_args(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "缺少 %s 的参数\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") {
      print_usage(argv[0]);
      return false;
    } else if (a == "--server") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.server = v;
    } else if (a == "--port") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.server_port = std::stoi(v);
    } else if (a == "--stream") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.stream_id = v;
    } else if (a == "--user") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.user_id = v;
    } else if (a == "--tcp") {
      opt.use_udp = false;
    } else if (a == "--out-ip") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.out_ip = v;
    } else if (a == "--out-port") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.out_port = std::stoi(v);
    } else if (a == "--mtu") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.mtu = std::stoi(v);
    } else if (a == "--dump-au") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.dump_au = v;
    } else if (a == "--no-pli") {
      opt.pli_once = false;
    } else if (a == "--ctrl-hz") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.ctrl_hz = std::stoi(v);
    } else if (a == "--seconds") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.seconds = std::stoi(v);
    } else if (a == "--verbose") {
      opt.verbose = true;
    } else {
      std::fprintf(stderr, "未知参数: %s\n", a.c_str());
      print_usage(argv[0]);
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, opt)) return 1;

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  std::signal(SIGPIPE, SIG_IGN);

  // RTP 转发 socket（connect 后 send 直达，等价 WFB 路径的同步 sendto）
  const int out_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (out_fd < 0) {
    std::fprintf(stderr, "[gnd] socket() 失败: %s\n", std::strerror(errno));
    return 1;
  }
  const int sndbuf = 4 * 1024 * 1024;
  ::setsockopt(out_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  struct sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(static_cast<uint16_t>(opt.out_port));
  if (::inet_pton(AF_INET, opt.out_ip.c_str(), &dst.sin_addr) != 1) {
    std::fprintf(stderr, "[gnd] 非法 out-ip: %s\n", opt.out_ip.c_str());
    return 1;
  }
  if (::connect(out_fd, reinterpret_cast<struct sockaddr*>(&dst),
                sizeof(dst)) < 0) {
    std::fprintf(stderr, "[gnd] connect(%s:%d) 失败: %s\n", opt.out_ip.c_str(),
                 opt.out_port, std::strerror(errno));
    return 1;
  }

  // AU 落盘（可选）
  std::FILE* dump = nullptr;
  if (!opt.dump_au.empty()) {
    dump = std::fopen(opt.dump_au.c_str(), "wb");
    if (dump == nullptr) {
      std::fprintf(stderr, "[gnd] fopen(%s) 失败: %s\n", opt.dump_au.c_str(),
                   std::strerror(errno));
      return 1;
    }
  }

  std::atomic<uint64_t> n_frames{0};
  std::atomic<uint64_t> n_params{0};
  std::atomic<uint64_t> n_idr{0};
  std::atomic<uint64_t> n_p{0};
  std::atomic<uint64_t> n_audio{0};
  std::atomic<uint64_t> n_unknown{0};
  std::atomic<uint64_t> n_rx_bytes{0};
  std::atomic<uint64_t> n_rtp_out{0};
  std::atomic<bool> connected{false};
  std::atomic<bool> pli_sent{false};
  std::atomic<bool> needs_restart{false};  // 订阅失败自动重试（P0 起要求）
  // L4 ctrl 计数：下行遥测按 MAVLink 帧解析分类；上行 RC/心跳
  std::atomic<uint64_t> n_ctrl_rx{0};
  std::atomic<uint64_t> n_ctrl_msgs{0};
  std::atomic<uint64_t> n_ctrl_hb{0};     // msgid=0
  std::atomic<uint64_t> n_ctrl_other{0};  // 其余（遥测等）
  std::atomic<uint64_t> n_ctrl_tx{0};
  std::atomic<uint32_t> last_ctrl_sysid{0};
  std::atomic<uint32_t> last_ctrl_msgid{0xFFFFFFFFu};

  // EsRtpPacketizer：Annex-B AU → RTP（逐包 send）
  EsRtpPacketizer pk(false, opt.mtu, [&](const uint8_t* rtp, size_t len) {
    n_rtp_out.fetch_add(1);
    const ssize_t sent = ::send(out_fd, rtp, len, 0);
    (void)sent;
  });

  // SDK Subscriber（unique_ptr + start_sub()：订阅失败后可整体重建重试）
  auto sub = std::make_unique<pull::Subscriber>();
  auto start_sub = [&]() {
    sub->SetOnConnected([&connected](uint32_t sid) {
      connected.store(true);
      std::printf("[gnd] CONNECTED sessionId=%u\n", sid);
      std::fflush(stdout);
    });
    sub->SetOnDisconnected([] {
      std::printf("[gnd] DISCONNECTED (SDK 将自动重连)\n");
      std::fflush(stdout);
    });
    sub->SetOnSubscribeFailed([&needs_restart](const std::string& reason) {
      std::printf("[gnd] SUBSCRIBE FAILED: %s (2s 后重建重试)\n",
                  reason.c_str());
      std::fflush(stdout);
      needs_restart.store(true);
    });
    // 下行 ctrl：OpenHD 聚合的 MAVLink 连续帧流，顺序解析计数
    sub->SetOnControlData([&](uint32_t /*src*/, const uint8_t* data, size_t len) {
      n_ctrl_rx.fetch_add(1);
      size_t off = 0;
      while (off + 8 <= len) {
        MavFrameInfo fi;
        if (!mav_parse(data + off, len - off, fi)) break;
        off += fi.frame_len;
        n_ctrl_msgs.fetch_add(1);
        last_ctrl_sysid.store(fi.sysid);
        last_ctrl_msgid.store(fi.msgid);
        if (fi.msgid == 0) n_ctrl_hb.fetch_add(1);
        else n_ctrl_other.fetch_add(1);
      }
    });
    sub->SetOnFrame([&](uint32_t /*src*/, std::vector<uint8_t>&& data) {
      if (data.empty()) return;
      const uint8_t type = data[0];
      const uint8_t* payload = data.data() + 1;
      const size_t len = data.size() - 1;

      if (type == 0x10) {  // 音频：计数（与 air 端发送数比对）
        n_audio.fetch_add(1);
        return;
      }
      if (type < 0x01 || type > 0x03) {
        n_unknown.fetch_add(1);
        return;
      }
      n_frames.fetch_add(1);
      n_rx_bytes.fetch_add(len);
      if (type == 0x01) n_params.fetch_add(1);
      else if (type == 0x02) n_idr.fetch_add(1);
      else n_p.fetch_add(1);

      if (dump != nullptr) std::fwrite(payload, 1, len, dump);
      if (opt.verbose) {
        std::printf("[gnd] frame #%llu type=0x%02X %zuB\n",
                    static_cast<unsigned long long>(n_frames.load()), type, len);
      }
      pk.on_au(type, payload, len);

      // 首帧到达说明 sourceSessionId 已就绪，此刻请求一次 PLI 验证该链路
      if (opt.pli_once && !pli_sent.exchange(true)) {
        sub->RequestKeyFrame();
        std::printf("[gnd] 已发送 PLI (关键帧请求), 观察 air 端探针日志\n");
        std::fflush(stdout);
      }
    });
    return sub->Start(opt.server, opt.server_port, opt.stream_id, opt.user_id,
                      opt.use_udp, 0);
  };

  if (!start_sub()) {
    std::fprintf(stderr, "[gnd] Subscriber Start 失败\n");
    return 1;
  }

  std::printf(
      "[gnd] 探针启动: SFU %s:%d (%s) stream=%s user=%s -> rtp %s:%d mtu=%d"
      " ctrl_hz=%d\n",
      opt.server.c_str(), opt.server_port, opt.use_udp ? "UDP" : "TCP",
      opt.stream_id.c_str(), opt.user_id.c_str(), opt.out_ip.c_str(),
      opt.out_port, opt.mtu, opt.ctrl_hz);
  std::fflush(stdout);

  const bool has_deadline = opt.seconds > 0;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(opt.seconds);
  auto last_print = std::chrono::steady_clock::now();
  auto last_restart_try = std::chrono::steady_clock::now();
  auto last_rc = std::chrono::steady_clock::now();
  auto last_hb = std::chrono::steady_clock::now();
  uint8_t rc_seq = 0;
  uint8_t hb_seq = 0;
  // 循环步长按 ctrl 速率取半，保证 --ctrl-hz 能真实达到（默认 100ms → 上限 10Hz）
  const auto tick_ms = std::chrono::milliseconds(
      opt.ctrl_hz > 10 ? std::max(10, 500 / opt.ctrl_hz) : 100);

  while (g_running.load()) {
    if (has_deadline && std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(tick_ms);

    const auto now = std::chrono::steady_clock::now();

    // 订阅失败重建（2s 冷却；Start 内部会先 Stop）
    if (needs_restart.exchange(false) &&
        now - last_restart_try >= std::chrono::seconds(2)) {
      last_restart_try = now;
      sub = std::make_unique<pull::Subscriber>();
      if (start_sub()) {
        std::printf("[gnd] 已重建订阅\n");
      } else {
        std::printf("[gnd] 重建订阅仍失败, 2s 后再试\n");
        needs_restart.store(true);
      }
      std::fflush(stdout);
    }

    // L4 上行 ctrl：RC_CHANNELS_OVERRIDE(70) 按 --ctrl-hz；HEARTBEAT(0) ~1Hz
    if (opt.ctrl_hz > 0 &&
        now - last_rc >=
            std::chrono::milliseconds(1000 / opt.ctrl_hz)) {
      last_rc = now;
      uint8_t payload[18] = {0};
      // chan1 慢速三角波 (1000..2000)，其余 1500 中位；target_system/component=0
      const uint32_t t = static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now.time_since_epoch()).count() / 1000);
      const uint16_t ch1 =
          static_cast<uint16_t>(1500 + ((t % 20) - 10) * 50);
      payload[0] = static_cast<uint8_t>(ch1 & 0xFF);
      payload[1] = static_cast<uint8_t>(ch1 >> 8);
      for (int c = 1; c < 8; ++c) {
        payload[c * 2] = static_cast<uint8_t>(1500 & 0xFF);
        payload[c * 2 + 1] = static_cast<uint8_t>(1500 >> 8);
      }
      uint8_t frame[32];
      const size_t flen =
          mav_build_v2(frame, 255, 190, 70, payload, 18, 124, rc_seq++);
      sub->SendCtrlUpstream(frame, flen, /*reliable=*/false);
      n_ctrl_tx.fetch_add(1);
    }
    if (opt.ctrl_hz > 0 &&
        now - last_hb >= std::chrono::seconds(1)) {
      last_hb = now;
      const uint8_t payload[9] = {0, 0, 0, 0, 6, 8, 0, 4, 3};  // GCS, 无飞控
      uint8_t frame[24];
      const size_t flen =
          mav_build_v2(frame, 255, 190, 0, payload, 9, 50, hb_seq++);
      sub->SendCtrlUpstream(frame, flen, /*reliable=*/false);
      n_ctrl_tx.fetch_add(1);
    }

    if (now - last_print >= std::chrono::seconds(1)) {
      last_print = now;
      const fec_protocol::ProtocolStats st = sub->GetStats();
      std::printf(
          "[gnd] 1s: frames=%llu (params=%llu idr=%llu p=%llu) audio=%llu "
          "unknown=%llu rtp_out=%llu rx_bytes=%llu | fec: done=%llu "
          "dropped=%llu lost=%llu rtt=%ums loss=%.1f%% fec_rec=%llu nack_rec=%llu"
          " | ctrl: rx_pkt=%llu msgs=%llu hb=%llu other=%llu last(sid=%u "
          "msgid=%d) tx=%llu\n",
          static_cast<unsigned long long>(n_frames.load()),
          static_cast<unsigned long long>(n_params.load()),
          static_cast<unsigned long long>(n_idr.load()),
          static_cast<unsigned long long>(n_p.load()),
          static_cast<unsigned long long>(n_audio.load()),
          static_cast<unsigned long long>(n_unknown.load()),
          static_cast<unsigned long long>(n_rtp_out.load()),
          static_cast<unsigned long long>(n_rx_bytes.load()),
          static_cast<unsigned long long>(st.frames_completed),
          static_cast<unsigned long long>(st.frames_dropped),
          static_cast<unsigned long long>(st.blocks_lost), st.rtt_ms,
          st.loss_rate * 100.0,
          static_cast<unsigned long long>(st.blocks_recovered_fec),
          static_cast<unsigned long long>(st.blocks_recovered_nack),
          static_cast<unsigned long long>(n_ctrl_rx.load()),
          static_cast<unsigned long long>(n_ctrl_msgs.load()),
          static_cast<unsigned long long>(n_ctrl_hb.load()),
          static_cast<unsigned long long>(n_ctrl_other.load()),
          last_ctrl_sysid.load(), static_cast<int>(last_ctrl_msgid.load()),
          static_cast<unsigned long long>(n_ctrl_tx.load()));
      std::fflush(stdout);
    }
  }

  sub->Stop();
  if (dump != nullptr) std::fclose(dump);
  ::close(out_fd);

  const fec_protocol::ProtocolStats st = sub->GetStats();
  std::printf(
      "[gnd] 结束: frames=%llu (params=%llu idr=%llu p=%llu) audio=%llu "
      "unknown=%llu rtp_out=%llu rx_bytes=%llu | fec: done=%llu dropped=%llu "
      "lost=%llu fec_rec=%llu nack_rec=%llu | ctrl: rx_pkt=%llu msgs=%llu "
      "hb=%llu other=%llu tx=%llu\n",
      static_cast<unsigned long long>(n_frames.load()),
      static_cast<unsigned long long>(n_params.load()),
      static_cast<unsigned long long>(n_idr.load()),
      static_cast<unsigned long long>(n_p.load()),
      static_cast<unsigned long long>(n_audio.load()),
      static_cast<unsigned long long>(n_unknown.load()),
      static_cast<unsigned long long>(n_rtp_out.load()),
      static_cast<unsigned long long>(n_rx_bytes.load()),
      static_cast<unsigned long long>(st.frames_completed),
      static_cast<unsigned long long>(st.frames_dropped),
      static_cast<unsigned long long>(st.blocks_lost),
      static_cast<unsigned long long>(st.blocks_recovered_fec),
      static_cast<unsigned long long>(st.blocks_recovered_nack),
      static_cast<unsigned long long>(n_ctrl_rx.load()),
      static_cast<unsigned long long>(n_ctrl_msgs.load()),
      static_cast<unsigned long long>(n_ctrl_hb.load()),
      static_cast<unsigned long long>(n_ctrl_other.load()),
      static_cast<unsigned long long>(n_ctrl_tx.load()));
  return 0;
}