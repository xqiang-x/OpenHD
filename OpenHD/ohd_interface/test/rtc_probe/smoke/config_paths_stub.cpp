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

// 测试替身：开发机无 /Config/（getConfigBasePath 硬编码），冒烟构建用本文件
// 替代仓库的 config_paths.cpp，把配置目录指向 RTC_SMOKE_CONFIG_DIR（默认
// /tmp/rtc_smoke/），由 rtc_config 读取其中的 rtc.txt。
#include <cstdlib>
#include <cstring>

#include "config_paths.h"

static char* CONFIG_BASE_PATH = nullptr;
static char* VIDEO_PATH = nullptr;

const char* getConfigBasePath() {
  if (CONFIG_BASE_PATH) return CONFIG_BASE_PATH;
  const char* env = getenv("RTC_SMOKE_CONFIG_DIR");
  return env ? env : "/tmp/rtc_smoke/";
}

const char* getVideoPath() { return "/tmp/rtc_smoke/"; }

void setConfigBasePath(const char* path) {
  if (CONFIG_BASE_PATH) free(CONFIG_BASE_PATH);
  CONFIG_BASE_PATH = strdup(path);
}

void setVideoPath(const char* path) {
  if (VIDEO_PATH) free(VIDEO_PATH);
  VIDEO_PATH = strdup(path);
}