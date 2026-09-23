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

// L0 离线自测：RtpEsAssembler（air：RTP→Annex-B AU）与 EsRtpPacketizer（ground：AU→RTP）
// 独立编译（不依赖 OpenHD 主构建，详见 openhd_rtc_link_design.md §10.1）：
//   g++ -std=c++17 -O2 -Wall -I OpenHD/OpenHD/ohd_interface/inc
//       OpenHD/OpenHD/ohd_interface/src/rtp_es_assembler.cpp
//       OpenHD/OpenHD/ohd_interface/src/es_rtp_packetizer.cpp
//       OpenHD/OpenHD/ohd_interface/test/test_rtp_es.cpp -o es_selftest
//
// 另含夹具模式（配合 gst 真实 RTP 回环，输出可用 ffprobe 校验）：
//   ./es_selftest --capture-udp <port> <seconds> <out.h264>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "es_rtp_packetizer.h"
#include "rtp_es_assembler.h"

namespace {

using Bytes = std::vector<uint8_t>;

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
      ++g_failures;                                                        \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
    }                                                                      \
  } while (0)

// ---------- 数据构造 ----------

// NALU: 1 字节头（F=0, NRI=3, type） + 确定性填充 payload
Bytes make_nalu(uint8_t type, size_t payload_len, uint8_t seed = 0x11) {
  Bytes n;
  n.reserve(1 + payload_len);
  n.push_back(static_cast<uint8_t>(0x60 | (type & 0x1F)));
  for (size_t i = 0; i < payload_len; ++i) {
    n.push_back(static_cast<uint8_t>(seed + (i % 251)));
  }
  return n;
}

Bytes annexb(const std::vector<Bytes>& nalus) {
  Bytes out;
  for (const auto& n : nalus) {
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);
    out.insert(out.end(), n.begin(), n.end());
  }
  return out;
}

// 手工构造 RTP 包（独立于 EsRtpPacketizer，避免"同源错误互相抵消"）
Bytes make_rtp(uint32_t ts, bool marker, const uint8_t* payload, size_t len) {
  Bytes p;
  p.reserve(12 + len);
  p.push_back(0x80);  // V=2
  p.push_back(static_cast<uint8_t>((marker ? 0x80 : 0x00) | 96));
  p.push_back(0xAB);  // seq（assembler 不依赖 seq）
  p.push_back(0xCD);
  p.push_back(static_cast<uint8_t>(ts >> 24));
  p.push_back(static_cast<uint8_t>((ts >> 16) & 0xFF));
  p.push_back(static_cast<uint8_t>((ts >> 8) & 0xFF));
  p.push_back(static_cast<uint8_t>(ts & 0xFF));
  p.push_back(0x11);  // ssrc
  p.push_back(0x22);
  p.push_back(0x33);
  p.push_back(0x44);
  p.insert(p.end(), payload, payload + len);
  return p;
}

// 把一个 NALU 手工切成 FU-A RTP 包序列
std::vector<Bytes> make_fu_a_packets(const Bytes& nalu, uint32_t ts,
                                     size_t frag_body, bool last_marker) {
  std::vector<Bytes> pkts;
  const uint8_t indicator = static_cast<uint8_t>((nalu[0] & 0xE0) | 28);
  size_t off = 1;
  bool first = true;
  while (off < nalu.size()) {
    const size_t chunk = std::min(frag_body, nalu.size() - off);
    const bool last = (off + chunk >= nalu.size());
    Bytes payload;
    payload.push_back(indicator);
    uint8_t fu = nalu[0] & 0x1F;
    if (first) {
      fu |= 0x80;
    }
    if (last) {
      fu |= 0x40;
    }
    payload.push_back(fu);
    payload.insert(payload.end(), nalu.begin() + static_cast<long>(off),
                   nalu.begin() + static_cast<long>(off + chunk));
    pkts.push_back(make_rtp(ts, last && last_marker, payload.data(), payload.size()));
    off += chunk;
    first = false;
  }
  return pkts;
}

// ---------- 测试夹具 ----------

struct AuOut {
  Bytes au;
  bool is_param;
  bool is_key;
};

struct TestAssembler {
  std::vector<AuOut> outs;
  std::vector<std::string> warns;
  RtpEsAssembler asm_;

  TestAssembler()
      : asm_(false, [this](Bytes&& au, bool is_param, bool is_key) {
          outs.push_back({std::move(au), is_param, is_key});
        }) {
    asm_.set_on_warn([this](const std::string& msg) { warns.push_back(msg); });
  }

  void feed(const Bytes& rtp) { asm_.on_rtp_packet(rtp.data(), rtp.size()); }
};

struct TestPacketizer {
  std::vector<Bytes> packets;
  EsRtpPacketizer pk_;

  TestPacketizer()
      : pk_(false, 1440, [this](const uint8_t* p, size_t n) {
          packets.push_back(Bytes(p, p + n));
        }) {}

  void send_au(uint8_t frame_type, const Bytes& au) {
    pk_.on_au(frame_type, au.data(), au.size());
  }
};

uint64_t now_us() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// ---------- 用例 ----------

void test_single_nalu_frames() {
  std::printf("=== Single NALU packets -> one AU per frame\n");
  TestAssembler t;
  uint32_t ts = 1000;
  std::vector<Bytes> expected;
  for (int i = 0; i < 3; ++i) {
    const Bytes nalu = make_nalu(1, 20 + static_cast<size_t>(i), 0x10);
    t.feed(make_rtp(ts, true, nalu.data(), nalu.size()));
    expected.push_back(annexb({nalu}));
    ts += 3000;
  }
  CHECK(t.outs.size() == 3);
  for (size_t i = 0; i < t.outs.size() && i < expected.size(); ++i) {
    CHECK(t.outs[i].au == expected[i]);
    CHECK(!t.outs[i].is_param);
    CHECK(!t.outs[i].is_key);
  }
}

void test_multi_slice_au() {
  std::printf("=== Multi-slice AU (same ts) merged in order\n");
  TestAssembler t;
  const Bytes s1 = make_nalu(1, 300, 0x21);
  const Bytes s2 = make_nalu(1, 120, 0x42);
  t.feed(make_rtp(5000, false, s1.data(), s1.size()));
  t.feed(make_rtp(5000, true, s2.data(), s2.size()));
  CHECK(t.outs.size() == 1);
  if (t.outs.size() == 1) {
    CHECK(t.outs[0].au == annexb({s1, s2}));
    CHECK(!t.outs[0].is_key);
  }
}

void test_params_idr_merged() {
  std::printf("=== SPS+PPS+SEI+IDR(FU-A) same ts -> one key AU\n");
  TestAssembler t;
  const uint32_t ts = 100;
  const Bytes sps = make_nalu(7, 12, 0x01);
  const Bytes pps = make_nalu(8, 6, 0x02);
  const Bytes sei = make_nalu(6, 8, 0x03);
  const Bytes idr = make_nalu(5, 5000, 0x04);

  t.feed(make_rtp(ts, false, sps.data(), sps.size()));
  t.feed(make_rtp(ts, false, pps.data(), pps.size()));
  t.feed(make_rtp(ts, false, sei.data(), sei.size()));
  for (const auto& pkt : make_fu_a_packets(idr, ts, 1426, true)) {
    t.feed(pkt);
  }

  CHECK(t.outs.size() == 1);
  if (t.outs.size() == 1) {
    CHECK(t.outs[0].au == annexb({sps, pps, idr}));  // SEI 被丢弃
    CHECK(!t.outs[0].is_param);
    CHECK(t.outs[0].is_key);
  }
}

void test_pure_params_au() {
  std::printf("=== Params-only AU emitted on ts change, is_param=true\n");
  TestAssembler t;
  const Bytes sps = make_nalu(7, 12, 0x05);
  const Bytes pps = make_nalu(8, 6, 0x06);
  const Bytes p = make_nalu(1, 80, 0x07);

  t.feed(make_rtp(200, false, sps.data(), sps.size()));
  t.feed(make_rtp(200, false, pps.data(), pps.size()));
  CHECK(t.outs.empty());
  t.feed(make_rtp(300, true, p.data(), p.size()));  // ts 变化收尾参数集 AU

  CHECK(t.outs.size() == 2);
  if (t.outs.size() == 2) {
    CHECK(t.outs[0].au == annexb({sps, pps}));
    CHECK(t.outs[0].is_param);
    CHECK(!t.outs[0].is_key);
    CHECK(t.outs[1].au == annexb({p}));
    CHECK(!t.outs[1].is_param);
  }
}

void test_timeout_flush() {
  std::printf("=== 500ms timeout flush emits open AU\n");
  TestAssembler t;
  const Bytes p = make_nalu(1, 64, 0x08);
  t.feed(make_rtp(400, false, p.data(), p.size()));
  CHECK(t.outs.empty());
  t.asm_.flush(now_us() + 600 * 1000);
  CHECK(t.outs.size() == 1);
  if (t.outs.size() == 1) {
    CHECK(t.outs[0].au == annexb({p}));
  }
}

void test_ts_change_mid_fu() {
  std::printf("=== ts change with incomplete FU drops fragment, next AU ok\n");
  TestAssembler t;
  const Bytes idr = make_nalu(5, 3000, 0x09);
  auto frags = make_fu_a_packets(idr, 600, 1426, true);
  CHECK(frags.size() >= 2);
  t.feed(frags[0]);  // 只发 S 片，随后 ts 变化
  const Bytes p = make_nalu(1, 50, 0x0A);
  t.feed(make_rtp(700, true, p.data(), p.size()));
  CHECK(t.outs.size() == 1);
  if (t.outs.size() == 1) {
    CHECK(t.outs[0].au == annexb({p}));
  }
}

void test_fu_desync() {
  std::printf("=== FU-A middle without start is dropped with warning\n");
  TestAssembler t;
  const Bytes idr = make_nalu(5, 400, 0x0B);
  // 先发一个无 S 的中间片（失步）
  {
    Bytes payload;
    payload.push_back(static_cast<uint8_t>((idr[0] & 0xE0) | 28));
    payload.push_back(idr[0] & 0x1F);  // S=0,E=0
    payload.insert(payload.end(), idr.begin() + 1, idr.end());
    t.feed(make_rtp(800, false, payload.data(), payload.size()));
  }
  CHECK(t.outs.empty());
  CHECK(!t.warns.empty());
  // 再发完整 FU 序列应正常
  for (const auto& pkt : make_fu_a_packets(idr, 800, 100, true)) {
    t.feed(pkt);
  }
  CHECK(t.outs.size() == 1);
  if (t.outs.size() == 1) {
    CHECK(t.outs[0].au == annexb({idr}));
    CHECK(t.outs[0].is_key);
  }
}

void test_stap_a() {
  std::printf("=== STAP-A with SPS+PPS -> params-only AU\n");
  TestAssembler t;
  const Bytes sps = make_nalu(7, 10, 0x0C);
  const Bytes pps = make_nalu(8, 5, 0x0D);
  Bytes payload;
  payload.push_back(static_cast<uint8_t>(0x60 | 24));  // STAP-A
  payload.push_back(0x00);
  payload.push_back(static_cast<uint8_t>(sps.size()));
  payload.insert(payload.end(), sps.begin(), sps.end());
  payload.push_back(0x00);
  payload.push_back(static_cast<uint8_t>(pps.size()));
  payload.insert(payload.end(), pps.begin(), pps.end());
  t.feed(make_rtp(900, true, payload.data(), payload.size()));
  CHECK(t.outs.size() == 1);
  if (t.outs.size() == 1) {
    CHECK(t.outs[0].au == annexb({sps, pps}));
    CHECK(t.outs[0].is_param);
    CHECK(!t.outs[0].is_key);
  }
}

void test_ext_header_and_padding() {
  std::printf("=== RTP extension header + padding are stripped\n");
  TestAssembler t;
  const Bytes p = make_nalu(1, 40, 0x0E);
  // b0 = V2 | X | P, CC=1；扩展 1 word；尾部 4 字节 padding
  Bytes pkt;
  pkt.push_back(0x80 | 0x10 | 0x20 | 0x01);
  pkt.push_back(0x80 | 96);
  pkt.push_back(0x00);
  pkt.push_back(0x01);
  pkt.push_back(0x00);
  pkt.push_back(0x00);
  pkt.push_back(0x27);
  pkt.push_back(0x10);
  pkt.push_back(0x11);
  pkt.push_back(0x22);
  pkt.push_back(0x33);
  pkt.push_back(0x44);
  pkt.push_back(0x00);  // CSRC
  pkt.push_back(0x00);
  pkt.push_back(0x00);
  pkt.push_back(0x01);
  pkt.push_back(0x00);  // ext profile
  pkt.push_back(0x00);
  pkt.push_back(0x00);  // ext len = 1 word
  pkt.push_back(0x01);
  pkt.push_back(0xDE);  // ext data
  pkt.push_back(0xAD);
  pkt.push_back(0xBE);
  pkt.push_back(0xEF);
  pkt.insert(pkt.end(), p.begin(), p.end());
  pkt.push_back(0x00);  // padding（最后字节=填充长度）
  pkt.push_back(0x00);
  pkt.push_back(0x00);
  pkt.push_back(0x04);
  t.feed(pkt);
  CHECK(t.outs.size() == 1);
  if (t.outs.size() == 1) {
    CHECK(t.outs[0].au == annexb({p}));
  }
}

void test_packetizer_boundaries() {
  std::printf("=== Packetizer: 1428 single / 1429 FU-A / marker semantics\n");
  TestPacketizer t;
  const Bytes n1428 = make_nalu(1, 1427);
  t.send_au(0x03, annexb({n1428}));
  CHECK(t.packets.size() == 1);
  if (t.packets.size() == 1) {
    CHECK(t.packets[0].size() == 12 + 1428);
    CHECK((t.packets[0][1] & 0x80) != 0);  // marker
    CHECK(std::equal(t.packets[0].begin() + 12, t.packets[0].end(),
                     n1428.begin()));
  }

  t.packets.clear();
  const Bytes n1429 = make_nalu(1, 1428);
  t.send_au(0x03, annexb({n1429}));
  CHECK(t.packets.size() == 2);
  if (t.packets.size() == 2) {
    // 末片才带 marker
    CHECK((t.packets[0][1] & 0x80) == 0);
    CHECK((t.packets[1][1] & 0x80) != 0);
    // 重组验证
    Bytes rebuilt;
    for (size_t i = 0; i < t.packets.size(); ++i) {
      const Bytes& pkt = t.packets[i];
      CHECK((pkt[12] & 0x1F) == 28);  // FU indicator
      const uint8_t fu = pkt[13];
      CHECK(((fu & 0x80) != 0) == (i == 0));                     // S
      CHECK(((fu & 0x40) != 0) == (i + 1 == t.packets.size()));  // E
      if (i == 0) {
        rebuilt.push_back(static_cast<uint8_t>((pkt[12] & 0xE0) | (fu & 0x1F)));
      }
      rebuilt.insert(rebuilt.end(), pkt.begin() + 14, pkt.end());
    }
    CHECK(rebuilt == n1429);
  }

  t.packets.clear();
  const Bytes sps = make_nalu(7, 10, 0x31);
  const Bytes pps = make_nalu(8, 5, 0x32);
  const Bytes p = make_nalu(1, 60, 0x33);
  t.send_au(0x02, annexb({sps, pps, p}));
  CHECK(t.packets.size() == 3);
  if (t.packets.size() == 3) {
    CHECK((t.packets[0][1] & 0x80) == 0);
    CHECK((t.packets[1][1] & 0x80) == 0);
    CHECK((t.packets[2][1] & 0x80) != 0);  // 仅 AU 末包 marker
    // 整个 AU 同一时间戳
    CHECK(t.packets[0][4] == t.packets[1][4] &&
          t.packets[1][4] == t.packets[2][4]);
  }
}

void test_roundtrip() {
  std::printf("=== Round trip: AU -> RTP -> AU byte-exact\n");
  TestPacketizer tp;
  const Bytes sps = make_nalu(7, 14, 0x41);
  const Bytes pps = make_nalu(8, 7, 0x42);
  const Bytes idr = make_nalu(5, 50000, 0x43);
  const Bytes p1 = make_nalu(1, 100, 0x44);
  const Bytes p2 = make_nalu(1, 60, 0x45);
  const Bytes pbig = make_nalu(1, 200000, 0x46);

  const Bytes au_idr = annexb({sps, pps, idr});
  const Bytes au_multi = annexb({p1, p2});
  const Bytes au_big = annexb({pbig});
  const Bytes au_params = annexb({sps, pps});

  tp.send_au(0x02, au_idr);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  tp.send_au(0x03, au_multi);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  tp.send_au(0x03, au_big);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  tp.send_au(0x01, au_params);
  CHECK(!tp.packets.empty());

  TestAssembler ta;
  for (const auto& pkt : tp.packets) {
    ta.feed(pkt);
  }
  CHECK(ta.outs.size() == 4);
  if (ta.outs.size() == 4) {
    CHECK(ta.outs[0].au == au_idr);
    CHECK(ta.outs[0].is_key);
    CHECK(!ta.outs[0].is_param);
    CHECK(ta.outs[1].au == au_multi);
    CHECK(!ta.outs[1].is_key);
    CHECK(ta.outs[2].au == au_big);
    CHECK(ta.outs[3].au == au_params);
    CHECK(ta.outs[3].is_param);
    CHECK(!ta.outs[3].is_key);
  }
}

void test_h265_weak_check() {
  std::printf("=== Consecutive invalid NALU types -> warn once + stop\n");
  TestAssembler t;
  const Bytes garbage = make_nalu(31, 10, 0x51);  // type 31：非法
  const Bytes p = make_nalu(1, 40, 0x52);

  for (int i = 0; i < 31; ++i) {
    t.feed(make_rtp(1000, false, garbage.data(), garbage.size()));
  }
  t.feed(make_rtp(1000, true, p.data(), p.size()));  // 有效包重置计数
  CHECK(!t.asm_.video_stopped());
  CHECK(t.outs.size() == 1);

  for (int i = 0; i < 32; ++i) {
    t.feed(make_rtp(2000, false, garbage.data(), garbage.size()));
  }
  CHECK(t.asm_.video_stopped());
  CHECK(t.warns.size() == 1);
  if (!t.warns.empty()) {
    CHECK(t.warns[0].find("H.265") != std::string::npos);
  }
  // 停止后不再产出
  const size_t out_before = t.outs.size();
  t.feed(make_rtp(3000, true, p.data(), p.size()));
  CHECK(t.outs.size() == out_before);
}

void test_au_too_big() {
  std::printf("=== AU > 4MB dropped with warning, subsequent AU ok\n");
  TestAssembler t;
  const Bytes big = make_nalu(1, 1024 * 1024, 0x61);
  for (int i = 0; i < 4; ++i) {
    t.feed(make_rtp(1000, false, big.data(), big.size()));
  }
  CHECK(t.outs.empty());
  CHECK(!t.warns.empty());
  const Bytes p = make_nalu(1, 40, 0x62);
  t.feed(make_rtp(2000, true, p.data(), p.size()));
  CHECK(t.outs.size() == 1);
  if (t.outs.size() == 1) {
    CHECK(t.outs[0].au == annexb({p}));
  }
}

// ---------- 夹具模式：捕获真实 RTP 流，装配为 Annex-B 落盘 ----------

int run_capture_udp(int port, int seconds, const char* out_path) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    std::fprintf(stderr, "socket() failed: %s\n", std::strerror(errno));
    return 1;
  }
  int rcvbuf = 8 * 1024 * 1024;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::fprintf(stderr, "bind(udp:%d) failed: %s\n", port, std::strerror(errno));
    ::close(fd);
    return 1;
  }
  std::FILE* out = std::fopen(out_path, "wb");
  if (out == nullptr) {
    std::fprintf(stderr, "fopen(%s) failed: %s\n", out_path, std::strerror(errno));
    ::close(fd);
    return 1;
  }

  uint64_t n_packets = 0;
  uint64_t n_aus = 0;
  uint64_t n_key = 0;
  uint64_t n_params = 0;
  uint64_t n_bytes = 0;
  RtpEsAssembler asmb(false, [&](Bytes&& au, bool is_param, bool is_key) {
    ++n_aus;
    if (is_key) ++n_key;
    if (is_param) ++n_params;
    n_bytes += au.size();
    std::fwrite(au.data(), 1, au.size(), out);
  });
  asmb.set_on_warn([](const std::string& msg) {
    std::fprintf(stderr, "[warn] %s\n", msg.c_str());
  });

  std::printf("capture: listening udp:%d for %ds -> %s\n", port, seconds,
              out_path);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  Bytes buf(65536);
  while (std::chrono::steady_clock::now() < deadline) {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, 100);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) {
      asmb.flush(now_us());
      continue;
    }
    const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
    if (n <= 0) continue;
    ++n_packets;
    asmb.on_rtp_packet(buf.data(), static_cast<size_t>(n));
  }
  // 收尾：+600ms 强制越过 500ms 超时判定，确保打开中的 AU 落盘
  asmb.flush(now_us() + 600 * 1000);
  std::fclose(out);
  ::close(fd);
  std::printf(
      "capture done: packets=%llu aus=%llu key=%llu params=%llu bytes=%llu\n",
      static_cast<unsigned long long>(n_packets),
      static_cast<unsigned long long>(n_aus),
      static_cast<unsigned long long>(n_key),
      static_cast<unsigned long long>(n_params),
      static_cast<unsigned long long>(n_bytes));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 5 && std::string(argv[1]) == "--capture-udp") {
    return run_capture_udp(std::atoi(argv[2]), std::atoi(argv[3]), argv[4]);
  }

  test_single_nalu_frames();
  test_multi_slice_au();
  test_params_idr_merged();
  test_pure_params_au();
  test_timeout_flush();
  test_ts_change_mid_fu();
  test_fu_desync();
  test_stap_a();
  test_ext_header_and_padding();
  test_packetizer_boundaries();
  test_roundtrip();
  test_h265_weak_check();
  test_au_too_big();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}