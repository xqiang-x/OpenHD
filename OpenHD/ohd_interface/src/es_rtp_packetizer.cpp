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

#include "es_rtp_packetizer.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace {

// 找 Annex-B 起始码（3/4 字节），返回起始码起始位置；与 NaluSplitter 同逻辑
std::ptrdiff_t find_start_code(const uint8_t* data, size_t size, size_t from,
                               int& sc_len) {
  if (size < 3) {
    return -1;
  }
  for (size_t i = from; i + 2 < size; ++i) {
    if (data[i] != 0x00 || data[i + 1] != 0x00) {
      continue;
    }
    if (data[i + 2] == 0x01) {
      sc_len = 3;
      return static_cast<std::ptrdiff_t>(i);
    }
    if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
      sc_len = 4;
      return static_cast<std::ptrdiff_t>(i);
    }
  }
  return -1;
}

}  // namespace

EsRtpPacketizer::EsRtpPacketizer(bool is_h265, int mtu, SendCb send_cb)
    : m_is_h265(is_h265),
      m_mtu(std::clamp(mtu, 64, 65535)),
      m_send_cb(std::move(send_cb)) {}

void EsRtpPacketizer::on_au(uint8_t frame_type, const uint8_t* data, size_t len) {
  if (!m_send_cb || data == nullptr || len < 4) {
    return;
  }
  if (frame_type != 0x01 && frame_type != 0x02 && frame_type != 0x03) {
    return;
  }

  // 1. 拆 NAL：保留 1..5（VCL）与 7/8（SPS/PPS），防御性过滤 SEI/AUD 等
  struct Nalu {
    const uint8_t* p;
    size_t len;
  };
  std::vector<Nalu> nalus;
  nalus.reserve(8);
  size_t pos = 0;
  int sc_len = 0;
  const auto first = find_start_code(data, len, pos, sc_len);
  if (first < 0) {
    return;
  }
  pos = static_cast<size_t>(first) + sc_len;
  while (pos < len) {
    int next_sc = 0;
    const auto next = find_start_code(data, len, pos, next_sc);
    const size_t nal_end = (next < 0) ? len : static_cast<size_t>(next);
    if (nal_end > pos) {
      const uint8_t type = data[pos] & 0x1F;
      if ((type >= 1 && type <= 5) || type == 7 || type == 8) {
        nalus.push_back({data + pos, nal_end - pos});
      }
    }
    if (next < 0) {
      break;
    }
    pos = nal_end + static_cast<size_t>(next_sc);
  }
  if (nalus.empty()) {
    return;
  }

  // 2. 打包：整个 AU 共用一个 RTP 时间戳；marker 打在最后一个包上
  m_cur_ts = next_ts_90k();
  for (size_t i = 0; i < nalus.size(); ++i) {
    const bool last = (i + 1 == nalus.size());
    if (nalus[i].len <= static_cast<size_t>(m_mtu) - kRtpHeaderSize) {
      send_single_nalu(nalus[i].p, nalus[i].len, last);
    } else {
      send_fu_a(nalus[i].p, nalus[i].len, last);
    }
  }
}

void EsRtpPacketizer::begin_packet(bool marker) {
  m_scratch.clear();
  m_scratch.push_back(0x80);  // V=2, P/X/CC=0
  m_scratch.push_back(
      static_cast<uint8_t>((marker ? 0x80 : 0x00) | kRtpPayloadType));
  m_scratch.push_back(static_cast<uint8_t>(m_seq >> 8));
  m_scratch.push_back(static_cast<uint8_t>(m_seq & 0xFF));
  ++m_seq;
  m_scratch.push_back(static_cast<uint8_t>(m_cur_ts >> 24));
  m_scratch.push_back(static_cast<uint8_t>((m_cur_ts >> 16) & 0xFF));
  m_scratch.push_back(static_cast<uint8_t>((m_cur_ts >> 8) & 0xFF));
  m_scratch.push_back(static_cast<uint8_t>(m_cur_ts & 0xFF));
  m_scratch.push_back(static_cast<uint8_t>(kSsrc >> 24));
  m_scratch.push_back(static_cast<uint8_t>((kSsrc >> 16) & 0xFF));
  m_scratch.push_back(static_cast<uint8_t>((kSsrc >> 8) & 0xFF));
  m_scratch.push_back(static_cast<uint8_t>(kSsrc & 0xFF));
}

void EsRtpPacketizer::finish_packet() {
  m_send_cb(m_scratch.data(), m_scratch.size());
}

void EsRtpPacketizer::send_single_nalu(const uint8_t* nalu, size_t len,
                                       bool marker) {
  begin_packet(marker);
  m_scratch.insert(m_scratch.end(), nalu, nalu + len);
  finish_packet();
}

void EsRtpPacketizer::send_fu_a(const uint8_t* nalu, size_t len, bool marker) {
  const size_t max_body = static_cast<size_t>(m_mtu) - kRtpHeaderSize - 2;
  const uint8_t indicator = static_cast<uint8_t>((nalu[0] & 0xE0) | 28);
  const uint8_t nal_type = nalu[0] & 0x1F;
  size_t off = 1;  // 原 NALU 头不进入 body（由 FU 头重建）
  bool first = true;
  while (off < len) {
    const size_t chunk = std::min(max_body, len - off);
    const bool last_frag = (off + chunk >= len);
    begin_packet(marker && last_frag);
    m_scratch.push_back(indicator);
    uint8_t fu = nal_type;
    if (first) {
      fu |= 0x80;
    }
    if (last_frag) {
      fu |= 0x40;
    }
    m_scratch.push_back(fu);
    m_scratch.insert(m_scratch.end(), nalu + off, nalu + off + chunk);
    finish_packet();
    off += chunk;
    first = false;
  }
}

uint32_t EsRtpPacketizer::next_ts_90k() {
  const uint64_t ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  return static_cast<uint32_t>((ns * 9ULL) / 100000ULL);
}