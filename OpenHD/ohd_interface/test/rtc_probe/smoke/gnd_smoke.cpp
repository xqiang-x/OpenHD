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

// M2 冒烟（L1.5）：RtcLink ground 真实类 —— Subscriber → AU→RTP → OHDLink 出口
// 编译/运行见同目录 build.sh / run_smoke.sh（开发机验证用）
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "openhd_profile.h"
#include "rtc_link.h"

int main(int argc, char** argv) {
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 60;
  OHDProfile profile(false, "smoke_gnd");
  RtcLink link(profile);

  std::atomic<uint64_t> pkts{0};
  std::atomic<uint64_t> aus{0};   // marker 包 = 一个 AU 的最后一个 RTP 包
  std::atomic<uint64_t> bad{0};   // RTP 头异常计数
  // M5：下行遥测/音频接收计数 + 上行 RC 发送计数
  std::atomic<uint64_t> telem_rx{0};
  std::atomic<uint64_t> rc_rx{0};
  std::atomic<uint64_t> audio_rx{0};
  std::atomic<uint64_t> rc_tx{0};
  link.register_on_receive_telemetry_data_cb(
      [&](std::shared_ptr<std::vector<uint8_t>> d) {
        if (d && !d->empty() && (*d)[0] == 'R') {
          rc_rx.fetch_add(1, std::memory_order_relaxed);
        } else {
          telem_rx.fetch_add(1, std::memory_order_relaxed);
        }
      });
  link.m_audio_data_rx_cb = [&](const uint8_t*, int) {
    audio_rx.fetch_add(1, std::memory_order_relaxed);
  };
  link.register_on_receive_video_data_cb(
      [&](int /*idx*/, const uint8_t* d, int len) {
        pkts.fetch_add(1, std::memory_order_relaxed);
        if (len < 12) {
          bad.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        if (((d[0] >> 6) & 0x3) != 2) bad.fetch_add(1, std::memory_order_relaxed);
        if ((d[1] & 0x7f) != 96) bad.fetch_add(1, std::memory_order_relaxed);
        if (d[1] & 0x80) aus.fetch_add(1, std::memory_order_relaxed);
      });

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  int tick = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ++tick;
    // 上行 RC：§4.3 推荐口径（UDP 不可靠 + n_injections=4 复制发送）
    auto r = std::make_shared<std::vector<uint8_t>>(30, 0);
    (*r)[0] = 'R';
    const uint32_t seq = static_cast<uint32_t>(rc_tx.load());
    std::memcpy(r->data() + 1, &seq, sizeof(seq));
    OHDLink::TelemetryTxPacket p;
    p.data = r;
    p.n_injections = 4;
    p.packet_type = OHDLink::TelemetryPacketType::RC;
    link.transmit_telemetry_data(p);
    rc_tx.fetch_add(1, std::memory_order_relaxed);
    if (tick % 10 == 0) {
      printf(
          "[gnd_smoke] rtp_pkts=%llu aus=%llu bad=%llu telem_rx=%llu rc_rx=%llu "
          "audio_rx=%llu rc_tx=%llu\n",
          static_cast<unsigned long long>(pkts.load()),
          static_cast<unsigned long long>(aus.load()),
          static_cast<unsigned long long>(bad.load()),
          static_cast<unsigned long long>(telem_rx.load()),
          static_cast<unsigned long long>(rc_rx.load()),
          static_cast<unsigned long long>(audio_rx.load()),
          static_cast<unsigned long long>(rc_tx.load()));
      fflush(stdout);
    }
  }
  printf(
      "[gnd_smoke] done: rtp_pkts=%llu aus=%llu bad=%llu telem_rx=%llu "
      "rc_rx=%llu audio_rx=%llu rc_tx=%llu\n",
      static_cast<unsigned long long>(pkts.load()),
      static_cast<unsigned long long>(aus.load()),
      static_cast<unsigned long long>(bad.load()),
      static_cast<unsigned long long>(telem_rx.load()),
      static_cast<unsigned long long>(rc_rx.load()),
      static_cast<unsigned long long>(audio_rx.load()),
      static_cast<unsigned long long>(rc_tx.load()));
  fflush(stdout);
  return 0;
}