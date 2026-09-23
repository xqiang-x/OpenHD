#!/bin/bash
# M4/L3 air 侧（pi4）：OpenHD air + RtcLink → SFU（stream openhd0）
# 部署：整个 OpenHD/OpenHD 源码到 pi4:~/OpenHD，构建：
#   cmake -S ~/OpenHD -B ~/OpenHD/build -DTJ_RTC_SDK_DIR=$HOME/rtc_l2/tj_rtc
#   make -j3
# 运行（需 root，OpenHD 会写 /usr/local/share/openhd 与 /Video）：
#   sudo ./run_ohd_air.sh -t 60        # 跑 60 秒
# 前置（在 pi4 上一次性准备，见 l3_air_openhd/README 记录）：
#   /Config/rtc.txt                     启用 RtcLink（server_ip 等）
#   /usr/local/share/openhd/video/air_camera_generic.json  相机类型=30 (ov5647)
# GST_PLUGIN_PATH: 用 /usr/local 的 libcamera 0.4 gst 插件（与 rpicam/pusher 同一套栈；
# 发行版的 0.5 插件与 /usr/local 的 libpisp 1.0.7 ABI 不兼容，加载会失败）
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
export GST_PLUGIN_PATH=/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0
cd "$DIR/OpenHD/build"
exec ./openhd --air "$@"