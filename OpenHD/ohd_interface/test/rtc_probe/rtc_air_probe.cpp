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

// L1 本机全链仿真探针（air 侧）——OpenHD air 视频路径的最小替身：
//   gst(rtph264pay) --UDP:5600--> [RtpEsAssembler] --> push::Publisher --> SFU
// 视频路径与 RtcLink 最终实现同源（同一 Assembler 实例化），仅遥测/RC/看护留待 M5。
// 与 gst 之间用裸 UDP 对接，不引入 gst 依赖。
//
// 构建（独立迷你 CMake，不进 OpenHD 主构建）：
//   cmake -S OpenHD/OpenHD/ohd_interface/test/rtc_probe -B /tmp/rtc_probe_build
//   cmake --build /tmp/rtc_probe_build -j
// 运行示例（配合 gst 源，见 openhd_rtc_link_design.md §10.2）：
//   ./rtc_air_probe --server 106.15.177.248 --stream rtcprobe1
//       --rtp-port 5600 --dump-au /tmp/l1_air.h264 --seconds 20
// 注意：CONNECTED 之前不要启动 gst 源（SDK 未注册时 PushVideoFrame 直接丢弃）。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "Publisher.h"
#include "rtp_es_assembler.h"

namespace {

std::atomic<bool> g_running{true};

void handle_signal(int /*sig*/) { g_running.store(false); }

uint64_t now_us() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct Options {
  int rtp_port = 5600;
  std::string server = "127.0.0.1";
  int server_port = 9200;
  std::string stream_id = "rtcprobe1";
  std::string user_id = "air_probe";
  bool use_udp = true;
  uint64_t pacer_mbps = 50;
  std::string dump_au;
  int seconds = 0;  // 0 = 无限
  bool verbose = false;
};

void print_usage(const char* prog) {
  std::fprintf(stderr,
               "Usage: %s [options]\n"
               "  --rtp-port PORT   收 gst RTP 的本地 UDP 端口 (默认 5600)\n"
               "  --server IP       SFU 服务器 (默认 127.0.0.1)\n"
               "  --port  PORT      SFU 端口 (默认 9200)\n"
               "  --stream  ID      流名 (默认 rtcprobe1)\n"
               "  --user  ID        用户 ID (默认 air_probe)\n"
               "  --tcp             使用 TCP 推流 (默认 UDP)\n"
               "  --bandwidth MBPS  UDP Pacer 带宽 Mbps (默认 50)\n"
               "  --dump-au PATH    装配后的 AU (Annex-B) 落盘, 供逐字节比对\n"
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
    } else if (a == "--rtp-port") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.rtp_port = std::stoi(v);
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
    } else if (a == "--bandwidth") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.pacer_mbps = std::stoull(v);
    } else if (a == "--dump-au") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.dump_au = v;
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

  // RTP 接收 socket
  const int rtp_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (rtp_fd < 0) {
    std::fprintf(stderr, "[air] socket() 失败: %s\n", std::strerror(errno));
    return 1;
  }
  const int rcvbuf = 8 * 1024 * 1024;
  ::setsockopt(rtp_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(opt.rtp_port));
  if (::bind(rtp_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) <
      0) {
    std::fprintf(stderr, "[air] bind(udp:%d) 失败: %s\n", opt.rtp_port,
                 std::strerror(errno));
    return 1;
  }

  // AU 落盘（可选）
  std::FILE* dump = nullptr;
  if (!opt.dump_au.empty()) {
    dump = std::fopen(opt.dump_au.c_str(), "wb");
    if (dump == nullptr) {
      std::fprintf(stderr, "[air] fopen(%s) 失败: %s\n", opt.dump_au.c_str(),
                   std::strerror(errno));
      return 1;
    }
  }

  std::atomic<bool> connected{false};
  std::atomic<uint64_t> n_packets{0};
  std::atomic<uint64_t> n_aus{0};
  std::atomic<uint64_t> n_key{0};
  std::atomic<uint64_t> n_params{0};
  std::atomic<uint64_t> n_au_bytes{0};
  std::atomic<uint64_t> n_warns{0};
  std::atomic<uint64_t> n_pli{0};

  // SDK Publisher
  push::Publisher pub;
  pub.SetBandwidth(opt.pacer_mbps * 1'000'000ULL);
  pub.SetOnConnected([&connected](uint32_t sid) {
    connected.store(true);
    std::printf("[air] CONNECTED sessionId=%u —— 现在可以启动 gst 源\n", sid);
    std::fflush(stdout);
  });
  pub.SetOnDisconnected([] {
    std::printf("[air] DISCONNECTED (SDK 将自动重连)\n");
    std::fflush(stdout);
  });
  pub.SetOnKeyFrameRequest([&n_pli] {
    // SDK 收包线程回调（持发送锁），只计数，主循环打印
    n_pli.fetch_add(1);
  });

  // RtpEsAssembler：RTP → Annex-B AU → PushVideoFrame
  RtpEsAssembler asmb(false, [&](std::vector<uint8_t>&& au, bool is_param,
                                 bool is_key) {
    if (dump != nullptr) std::fwrite(au.data(), 1, au.size(), dump);
    n_aus.fetch_add(1);
    if (is_key) n_key.fetch_add(1);
    if (is_param) n_params.fetch_add(1);
    n_au_bytes.fetch_add(au.size());
    if (opt.verbose) {
      std::printf("[air] AU #%llu %zuB param=%d key=%d\n",
                  static_cast<unsigned long long>(n_aus.load()), au.size(),
                  is_param ? 1 : 0, is_key ? 1 : 0);
    }
    pub.PushVideoFrame(au.data(), au.size(), is_param, is_key);
  });
  asmb.set_on_warn([&n_warns](const std::string& msg) {
    n_warns.fetch_add(1);
    std::fprintf(stderr, "[air][warn] %s\n", msg.c_str());
  });

  if (!pub.Start(opt.server, opt.server_port, opt.stream_id, opt.user_id,
                 opt.use_udp, 0)) {
    std::fprintf(stderr, "[air] Publisher Start 失败\n");
    return 1;
  }

  std::printf(
      "[air] 探针启动: rtp=udp:%d -> SFU %s:%d (%s) stream=%s user=%s\n"
      "[air] 等待 CONNECTED 后再启动 gst 源 (rtph264pay -> 127.0.0.1:%d)\n",
      opt.rtp_port, opt.server.c_str(), opt.server_port,
      opt.use_udp ? "UDP" : "TCP", opt.stream_id.c_str(), opt.user_id.c_str(),
      opt.rtp_port);
  std::fflush(stdout);

  const bool has_deadline = opt.seconds > 0;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(opt.seconds);
  auto last_print = std::chrono::steady_clock::now();
  uint64_t last_pli_seen = 0;
  std::vector<uint8_t> buf(65536);
  auto last_packets = n_packets.load();
  auto last_aus = n_aus.load();

  while (g_running.load()) {
    if (has_deadline && std::chrono::steady_clock::now() >= deadline) break;

    if (!connected.load()) {
      // 未注册时 PushVideoFrame 会被丢弃——不消费 RTP，保持字节序列完整
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    struct pollfd pfd{};
    pfd.fd = rtp_fd;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, 200);
    if (pr > 0 && (pfd.revents & POLLIN) != 0) {
      const ssize_t n = ::recv(rtp_fd, buf.data(), buf.size(), 0);
      if (n > 0) {
        n_packets.fetch_add(1);
        asmb.on_rtp_packet(buf.data(), static_cast<size_t>(n));
      }
    } else if (pr == 0) {
      asmb.flush(now_us());
    }

    const uint64_t pli = n_pli.load();
    if (pli != last_pli_seen) {
      std::printf(
          "[air] 收到 PLI 关键帧请求 (累计 %llu) —— 真实实现此处强制下一帧 IDR\n",
          static_cast<unsigned long long>(pli));
      last_pli_seen = pli;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_print >= std::chrono::seconds(1)) {
      last_print = now;
      const uint64_t pkts = n_packets.load();
      const uint64_t aus = n_aus.load();
      std::printf("[air] 1s: rtp_pkts=%llu(+%llu) aus=%llu(+%llu) key=%llu "
                  "au_bytes=%llu pli=%llu warns=%llu\n",
                  static_cast<unsigned long long>(pkts),
                  static_cast<unsigned long long>(pkts - last_packets),
                  static_cast<unsigned long long>(aus),
                  static_cast<unsigned long long>(aus - last_aus),
                  static_cast<unsigned long long>(n_key.load()),
                  static_cast<unsigned long long>(n_au_bytes.load()),
                  static_cast<unsigned long long>(pli),
                  static_cast<unsigned long long>(n_warns.load()));
      last_packets = pkts;
      last_aus = aus;
    }
  }

  // 收尾：+600ms 强制越过 500ms 超时判定，确保打开中的 AU 落盘并推流；
  // 再留 500ms 让 Publisher 发送队列排空，保证字节比对的完整性
  asmb.flush(now_us() + 600 * 1000);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  pub.Stop();
  if (dump != nullptr) std::fclose(dump);
  ::close(rtp_fd);

  std::printf(
      "[air] 结束: rtp_pkts=%llu aus=%llu key=%llu params=%llu au_bytes=%llu "
      "pli=%llu warns=%llu\n",
      static_cast<unsigned long long>(n_packets.load()),
      static_cast<unsigned long long>(n_aus.load()),
      static_cast<unsigned long long>(n_key.load()),
      static_cast<unsigned long long>(n_params.load()),
      static_cast<unsigned long long>(n_au_bytes.load()),
      static_cast<unsigned long long>(n_pli.load()),
      static_cast<unsigned long long>(n_warns.load()));
  return 0;
}