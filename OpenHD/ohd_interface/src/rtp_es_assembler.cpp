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

#include "rtp_es_assembler.h"

#include <chrono>
#include <utility>

namespace {

constexpr size_t kRtpHeaderMinSize = 12;
constexpr size_t kAnnexBStartCodeSize = 4;

uint16_t read_be16(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t read_be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void append_annexb(std::vector<uint8_t>& dst, const uint8_t* nalu, size_t len) {
  static const uint8_t start_code[kAnnexBStartCodeSize] = {0, 0, 0, 1};
  dst.insert(dst.end(), start_code, start_code + kAnnexBStartCodeSize);
  dst.insert(dst.end(), nalu, nalu + len);
}

}  // namespace

RtpEsAssembler::RtpEsAssembler(bool is_h265, OutCb out_cb)
    : m_is_h265(is_h265), m_out_cb(std::move(out_cb)) {}

void RtpEsAssembler::set_on_warn(WarnCb warn_cb) {
  m_warn_cb = std::move(warn_cb);
}

uint64_t RtpEsAssembler::now_us() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void RtpEsAssembler::warn(const std::string& msg) {
  if (m_warn_cb) {
    m_warn_cb(msg);
  }
}

void RtpEsAssembler::on_rtp_packet(const uint8_t* pkt, size_t len) {
  if (m_video_stopped) {
    return;
  }
  if (len < kRtpHeaderMinSize) {
    return;
  }
  const uint64_t now = now_us();
  if (m_au_open && now - m_au_start_us >= kAuTimeoutUs) {
    finalize_au();
  }

  const uint8_t b0 = pkt[0];
  if ((b0 >> 6) != 2) {
    return;  // V != 2：非法 RTP 包
  }
  const bool has_padding = (b0 & 0x20) != 0;
  const bool has_extension = (b0 & 0x10) != 0;
  const size_t cc = b0 & 0x0F;
  const bool marker = (pkt[1] & 0x80) != 0;
  const uint32_t ts = read_be32(pkt + 4);

  size_t header_size = kRtpHeaderMinSize + 4 * cc;
  if (has_extension) {
    if (len < header_size + 4) {
      return;
    }
    const uint32_t ext_words = read_be16(pkt + header_size + 2);
    header_size += 4 + 4 * ext_words;
  }
  if (len <= header_size) {
    return;
  }
  const uint8_t* payload = pkt + header_size;
  size_t payload_len = len - header_size;
  if (has_padding) {
    if (payload_len < 1) {
      return;
    }
    const uint8_t pad = payload[payload_len - 1];
    if (pad == 0 || pad > payload_len) {
      return;
    }
    payload_len -= pad;
  }
  if (payload_len == 0) {
    return;
  }

  // 成帧：timestamp 变化时收尾旧 AU，并丢弃可能残留的未完成 FU 分片
  if (ts != m_last_seen_ts) {
    if (m_au_open) {
      finalize_au();
    }
    if (m_fu_in_progress) {
      warn("RTP FU-A: timestamp changed with incomplete FU, fragment dropped");
      m_fu_in_progress = false;
      m_fu_nalu.clear();
    }
  }
  m_last_seen_ts = ts;

  const uint8_t nal_type = payload[0] & 0x1F;
  const bool valid_type =
      (nal_type >= 1 && nal_type <= 23) || nal_type == 24 || nal_type == 28;
  if (valid_type) {
    reset_invalid_count();
  } else {
    // 25/26/27/29/30/31 等：H.264 中不使用或 v1 不支持（29-31 为 FU-B/STAP-B/MTAP）
    note_invalid_nalu_type();
    return;
  }

  if (nal_type <= 23) {  // 单 NALU 包
    emit_nalu(payload, payload_len);
  } else if (nal_type == 28) {  // FU-A
    if (payload_len < 2) {
      return;
    }
    const uint8_t fu = payload[1];
    const bool s = (fu & 0x80) != 0;
    const bool e = (fu & 0x40) != 0;
    if (s) {
      if (m_fu_in_progress) {
        warn("RTP FU-A: start while previous FU incomplete, previous dropped");
      }
      m_fu_in_progress = true;
      m_fu_nalu.clear();
      m_fu_nalu.push_back(static_cast<uint8_t>((payload[0] & 0xE0) | (fu & 0x1F)));
      m_fu_nalu.insert(m_fu_nalu.end(), payload + 2, payload + payload_len);
    } else {
      if (!m_fu_in_progress) {
        warn("RTP FU-A: middle/end fragment without start, packet dropped");
        return;
      }
      m_fu_nalu.insert(m_fu_nalu.end(), payload + 2, payload + payload_len);
    }
    if (m_fu_nalu.size() > kMaxAuBytes) {
      warn("RTP FU-A: reassembly exceeds 4MB, fragment dropped");
      m_fu_nalu.clear();
      m_fu_in_progress = false;
      return;
    }
    if (e) {
      m_fu_in_progress = false;
      emit_nalu(m_fu_nalu.data(), m_fu_nalu.size());
      m_fu_nalu.clear();
    }
  } else {  // STAP-A（防御性；gstreamer rtph264pay 默认不产出）
    size_t off = 1;
    while (off + 2 <= payload_len) {
      const size_t nalu_len = read_be16(payload + off);
      off += 2;
      if (off + nalu_len > payload_len) {
        warn("RTP STAP-A malformed, packet dropped");
        return;
      }
      if (nalu_len > 0) {
        emit_nalu(payload + off, nalu_len);
      }
      off += nalu_len;
    }
  }

  if (marker) {
    finalize_au();
  }
}

void RtpEsAssembler::flush(uint64_t now) {
  if (m_au_open && now - m_au_start_us >= kAuTimeoutUs) {
    finalize_au();
  }
}

void RtpEsAssembler::emit_nalu(const uint8_t* nalu, size_t len) {
  if (len == 0) {
    return;
  }
  const uint8_t nal_type = nalu[0] & 0x1F;
  const bool is_vcl = nal_type >= 1 && nal_type <= 5;
  const bool is_param = nal_type == 7 || nal_type == 8;
  if (!is_vcl && !is_param) {
    return;  // 6/9/12（SEI/AUD/Filler）等：分组边界，丢弃
  }

  open_au_if_needed();

  const size_t add = len + kAnnexBStartCodeSize;
  if (m_au_bytes + add > kMaxAuBytes) {
    warn("AU exceeds 4MB, current AU dropped");
    m_params.clear();
    m_vcl.clear();
    m_fu_nalu.clear();
    m_fu_in_progress = false;
    m_has_idr = false;
    m_au_open = false;
    m_au_bytes = 0;
    return;
  }
  m_au_bytes += add;
  if (is_vcl) {
    m_vcl.emplace_back(nalu, nalu + len);
    if (nal_type == 5) {
      m_has_idr = true;
    }
  } else {
    m_params.emplace_back(nalu, nalu + len);
  }
}

void RtpEsAssembler::open_au_if_needed() {
  if (m_au_open) {
    return;
  }
  m_au_open = true;
  m_cur_ts = m_last_seen_ts;
  m_au_start_us = now_us();
  m_au_bytes = 0;
  m_has_idr = false;
}

void RtpEsAssembler::finalize_au() {
  if (!m_au_open) {
    return;
  }
  if (m_fu_in_progress) {
    warn("AU finalized with incomplete FU-A fragment, fragment dropped");
  }
  m_fu_in_progress = false;
  m_fu_nalu.clear();

  const bool has_vcl = !m_vcl.empty();
  if (has_vcl || !m_params.empty()) {
    std::vector<uint8_t> au;
    size_t total = 0;
    for (const auto& n : m_params) {
      total += n.size() + kAnnexBStartCodeSize;
    }
    for (const auto& n : m_vcl) {
      total += n.size() + kAnnexBStartCodeSize;
    }
    au.reserve(total);
    for (const auto& n : m_params) {
      append_annexb(au, n.data(), n.size());
    }
    for (const auto& n : m_vcl) {
      append_annexb(au, n.data(), n.size());
    }
    const bool is_key = m_has_idr;
    const bool is_param = !has_vcl;  // 纯参数集 AU
    if (m_out_cb) {
      m_out_cb(std::move(au), is_param, is_key);
    }
  }

  m_params.clear();
  m_vcl.clear();
  m_has_idr = false;
  m_au_open = false;
  m_au_bytes = 0;
}

void RtpEsAssembler::note_invalid_nalu_type() {
  if (++m_consecutive_invalid >= kMaxConsecutiveInvalid && !m_video_stopped) {
    m_video_stopped = true;
    warn(
        "Suspected H.265 or non-H.264 stream (consecutive invalid NALU "
        "types), stopping video. v1 supports H.264 only");
  }
}