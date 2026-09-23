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

#include "rtc_config.h"

#include <cctype>
#include <sstream>

#include "config_paths.h"
#include "openhd_spdlog.h"
#include "openhd_spdlog_include.h"
#include "openhd_util.h"
#include "openhd_util_filesystem.h"

namespace {

std::string to_lower(std::string s) {
  for (auto& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

bool parse_bool(const std::string& v, bool& out) {
  const auto s = to_lower(v);
  if (s == "1" || s == "true" || s == "yes" || s == "on") {
    out = true;
    return true;
  }
  if (s == "0" || s == "false" || s == "no" || s == "off") {
    out = false;
    return true;
  }
  return false;
}

bool parse_int(const std::string& v, int& out) {
  try {
    size_t pos = 0;
    const auto tmp = std::stoi(v, &pos);
    if (pos != v.size()) return false;
    out = tmp;
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_u64(const std::string& v, uint64_t& out) {
  try {
    size_t pos = 0;
    const auto tmp = std::stoull(v, &pos);
    if (pos != v.size()) return false;
    out = tmp;
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

RtcConfig RtcConfig::load() {
  return load_from_file(std::string(getConfigBasePath()) + "rtc.txt");
}

RtcConfig RtcConfig::load_from_file(const std::string& path) {
  auto console = openhd::log::create_or_get("rtc_cfg");
  RtcConfig cfg;
  const auto content = OHDFilesystemUtil::read_file(path);
  if (content.empty()) {
    console->warn("rtc.txt 不存在或为空: {}", path);
    return cfg;
  }
  std::istringstream stream(content);
  std::string line;
  int line_no = 0;
  while (std::getline(stream, line)) {
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    OHDUtil::trim(line);
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      console->warn("rtc.txt:{} 忽略无 '=' 的行: {}", line_no, line);
      continue;
    }
    std::string key = line.substr(0, eq);
    std::string value = line.substr(eq + 1);
    OHDUtil::trim(key);
    OHDUtil::trim(value);
    const auto k = to_lower(key);
    if (k == "server_ip") {
      cfg.server_ip = value;
    } else if (k == "tcp_port") {
      int v = cfg.tcp_port;
      if (parse_int(value, v) && v >= 1 && v <= 65535) {
        cfg.tcp_port = v;
      } else {
        console->warn("rtc.txt:{} tcp_port 非法 '{}'，保持默认 {}", line_no,
                      value, cfg.tcp_port);
      }
    } else if (k == "udp_port") {
      int v = cfg.udp_port;
      if (parse_int(value, v) && v >= 0 && v <= 65535) {
        cfg.udp_port = v;
      } else {
        console->warn("rtc.txt:{} udp_port 非法 '{}'，保持默认 {}", line_no,
                      value, cfg.udp_port);
      }
    } else if (k == "use_udp") {
      bool v = cfg.use_udp;
      if (parse_bool(value, v)) {
        cfg.use_udp = v;
      } else {
        console->warn("rtc.txt:{} use_udp 非法 '{}'，保持默认 {}", line_no,
                      value, cfg.use_udp ? 1 : 0);
      }
    } else if (k == "stream_prefix") {
      if (!value.empty()) {
        cfg.stream_prefix = value;
      } else {
        console->warn("rtc.txt:{} stream_prefix 为空，保持默认 '{}'", line_no,
                      cfg.stream_prefix);
      }
    } else if (k == "user_id_air") {
      if (!value.empty()) {
        cfg.user_id_air = value;
      } else {
        console->warn("rtc.txt:{} user_id_air 为空，保持默认 '{}'", line_no,
                      cfg.user_id_air);
      }
    } else if (k == "user_id_ground") {
      if (!value.empty()) {
        cfg.user_id_ground = value;
      } else {
        console->warn("rtc.txt:{} user_id_ground 为空，保持默认 '{}'", line_no,
                      cfg.user_id_ground);
      }
    } else if (k == "pacer_mbps") {
      uint64_t v = cfg.pacer_mbps;
      if (parse_u64(value, v) && v >= 1 && v <= 10000) {
        cfg.pacer_mbps = v;
      } else {
        console->warn("rtc.txt:{} pacer_mbps 非法 '{}'，保持默认 {}", line_no,
                      value, cfg.pacer_mbps);
      }
    } else if (k == "enable_cam0" || k == "enable_cam1") {
      bool v = false;
      const int idx = k.back() - '0';
      if (parse_bool(value, v)) {
        cfg.enable_cam[idx] = v;
      } else {
        console->warn("rtc.txt:{} {} 非法 '{}'，保持默认 {}", line_no, key,
                      value, cfg.enable_cam[idx] ? 1 : 0);
      }
    } else if (k == "rc_reliable") {
      bool v = cfg.rc_reliable;
      if (parse_bool(value, v)) {
        cfg.rc_reliable = v;
      } else {
        console->warn("rtc.txt:{} rc_reliable 非法 '{}'，保持默认 {}", line_no,
                      value, cfg.rc_reliable ? 1 : 0);
      }
    } else if (k == "telemetry_reliable") {
      bool v = cfg.telemetry_reliable;
      if (parse_bool(value, v)) {
        cfg.telemetry_reliable = v;
      } else {
        console->warn("rtc.txt:{} telemetry_reliable 非法 '{}'，保持默认 {}",
                      line_no, value, cfg.telemetry_reliable ? 1 : 0);
      }
    } else {
      console->warn("rtc.txt:{} 未知配置键 '{}'，已忽略", line_no, key);
    }
  }
  if (!cfg.is_valid()) {
    console->error("rtc.txt 缺少必填项 server_ip: {}", path);
  } else {
    console->warn(
        "rtc 配置: server={}:{} udp={} stream_prefix={} cam={}/{} pacer={}Mbps",
        cfg.server_ip, cfg.tcp_port, cfg.use_udp ? "yes" : "no",
        cfg.stream_prefix, cfg.enable_cam[0], cfg.enable_cam[1],
        cfg.pacer_mbps);
  }
  return cfg;
}