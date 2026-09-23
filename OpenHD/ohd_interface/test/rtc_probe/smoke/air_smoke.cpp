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

// M2 冒烟（L1.5）：RtcLink air 真实类 —— gst RTP(UDP) → transmit_video_data → SFU
// 编译/运行见同目录 build.sh / run_smoke.sh（开发机验证用）
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "openhd_profile.h"
#include "rtc_link.h"

int main(int argc, char** argv) {
  const int listen_port = argc > 1 ? std::atoi(argv[1]) : 15997;
  const int seconds = argc > 2 ? std::atoi(argv[2]) : 30;
  OHDProfile profile(true, "smoke_air");
  RtcLink link(profile);

  // M5：下行遥测/音频发送 + 上行 RC 接收计数（P1 使上行窗口不依赖媒体帧）
  const auto t_start = std::chrono::steady_clock::now();
  std::atomic<uint64_t> ctrl_rx{0};
  std::atomic<uint64_t> first_ctrl_ms{0};
  link.register_on_receive_telemetry_data_cb(
      [&](std::shared_ptr<std::vector<uint8_t>> data) {
        const auto n = ctrl_rx.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1) {
          const auto ms = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t_start)
                  .count());
          first_ctrl_ms.store(ms, std::memory_order_relaxed);
          printf("[air_smoke] first upstream ctrl at t=%llums (%llu bytes)\n",
                 static_cast<unsigned long long>(ms),
                 static_cast<unsigned long long>(data ? data->size() : 0));
          fflush(stdout);
        }
      });

  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    perror("socket");
    return 1;
  }
  int rcvbuf = 8 * 1024 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<uint16_t>(listen_port));
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }
  timeval tv{0, 200 * 1000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  std::vector<std::shared_ptr<std::vector<uint8_t>>> frags;
  uint64_t packets = 0;
  uint64_t frames = 0;
  uint64_t telem_tx = 0;
  uint64_t audio_tx = 0;
  uint32_t tx_seq = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  auto last_stats = std::chrono::steady_clock::now();
  auto next_tx = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < deadline) {
    uint8_t buf[2048];
    const auto n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      // 超时即到发送节拍检查
    } else {
      const bool marker = n > 1 && (buf[1] & 0x80);
      frags.push_back(std::make_shared<std::vector<uint8_t>>(buf, buf + n));
      ++packets;
      if (marker) {
        openhd::FragmentedVideoFrame frame;
        frame.rtp_fragments = std::move(frags);
        frags.clear();
        link.transmit_video_data(0, frame);
        ++frames;
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_tx) {
      next_tx = now + std::chrono::milliseconds(100);
      // 下行遥测（模拟 mavlink 聚合帧，24B，unreliable 单发）
      auto t = std::make_shared<std::vector<uint8_t>>(24, 0);
      (*t)[0] = 'T';
      std::memcpy(t->data() + 1, &tx_seq, sizeof(tx_seq));
      OHDLink::TelemetryTxPacket tp;
      tp.data = t;
      tp.n_injections = 1;
      tp.packet_type = OHDLink::TelemetryPacketType::Telemetry;
      link.transmit_telemetry_data(tp);
      // 下行音频（伪 PCMA RTP 包：12B 头 + 160B 载荷，不透明透传）
      auto a = std::make_shared<std::vector<uint8_t>>(172, 0);
      (*a)[0] = 0x80;
      (*a)[1] = 96;
      std::memcpy(a->data() + 12, &tx_seq, sizeof(tx_seq));
      openhd::AudioPacket ap;
      ap.data = a;
      link.transmit_audio_data(ap);
      ++tx_seq;
      ++telem_tx;
      ++audio_tx;
    }
    if (std::chrono::steady_clock::now() - last_stats >
        std::chrono::seconds(5)) {
      last_stats = std::chrono::steady_clock::now();
      printf("[air_smoke] packets=%llu frames=%llu telem_tx=%llu audio_tx=%llu "
             "ctrl_rx=%llu\n",
             static_cast<unsigned long long>(packets),
             static_cast<unsigned long long>(frames),
             static_cast<unsigned long long>(telem_tx),
             static_cast<unsigned long long>(audio_tx),
             static_cast<unsigned long long>(ctrl_rx.load()));
      fflush(stdout);
    }
  }
  printf("[air_smoke] done: packets=%llu frames=%llu telem_tx=%llu audio_tx=%llu "
         "ctrl_rx=%llu first_ctrl_ms=%llu\n",
         static_cast<unsigned long long>(packets),
         static_cast<unsigned long long>(frames),
         static_cast<unsigned long long>(telem_tx),
         static_cast<unsigned long long>(audio_tx),
         static_cast<unsigned long long>(ctrl_rx.load()),
         static_cast<unsigned long long>(first_ctrl_ms.load()));
  fflush(stdout);
  return 0;
}