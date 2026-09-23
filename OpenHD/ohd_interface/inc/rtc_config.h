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

#ifndef OPENHD_RTC_CONFIG_H
#define OPENHD_RTC_CONFIG_H

#include <array>
#include <cstdint>
#include <string>

/**
 * RtcLink 配置：getConfigBasePath() + "rtc.txt"，仿 ethernet.txt——文件存在即
 * 启用 RtcLink（见 openhd_rtc_link_design.md §6）。
 * 解析规则：key=value、# 注释行、去空白；非法值回退默认并告警。
 */
struct RtcConfig {
  std::string server_ip;                 // 必填：SFU 服务器地址
  int tcp_port = 9200;                   // 信令端口
  int udp_port = 0;                      // 0 → tcp_port + 1
  bool use_udp = true;                   // true=UDP+FEC（推荐）；false=TCP（仅调试）
  std::string stream_prefix = "openhd";  // streamId = prefix + 相机索引（openhd0）
  std::string user_id_air = "ohd_air";
  std::string user_id_ground = "ohd_gnd";
  uint64_t pacer_mbps = 50;              // UDP 模式发送带宽上限
  std::array<bool, 2> enable_cam{true, false};  // 各相机是否走 RTC 链路
  bool rc_reliable = false;          // false=UDP 按 n_injections 复制发送（默认）
  bool telemetry_reliable = false;   // 同上，遥测通道

  static RtcConfig load();

  // 解析指定文件；文件不存在/为空时返回全默认（is_valid()==false）
  static RtcConfig load_from_file(const std::string& path);

  bool is_valid() const { return !server_ip.empty(); }

  int effective_udp_port() const {
    return udp_port > 0 ? udp_port : tcp_port + 1;
  }
};

#endif  // OPENHD_RTC_CONFIG_H