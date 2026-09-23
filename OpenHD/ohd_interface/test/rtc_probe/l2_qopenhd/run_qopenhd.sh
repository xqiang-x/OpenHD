#!/bin/bash
# L2 出画验证：启动本地安装（无 root）的 QOpenHD
# 前置：install_qopenhd_noroot.sh 已装到 $QOPENHD_PREFIX/root（默认 /tmp/qopenhd_local）
# 视频来源（二选一）：
#   1) 标准拓扑：任意进程把 RTP 转发到本机 127.0.0.1:5600（QOpenHD 主视频默认端口，
#      其 avcodec 接收路径绑定 0.0.0.0，LAN 直发本机 IP:5600 亦可）
#   2) 实机对照：pi4 上 rtc_gnd_probe 订阅 pivideo 后
#      --out-ip <本机IP> --out-port 5600 直接灌给 QOpenHD（见设计文档 §10.3 L2 记录）
set -e
PREFIX="${QOPENHD_PREFIX:-/tmp/qopenhd_local}"
ROOT="$PREFIX/root"
export LD_LIBRARY_PATH="$ROOT/usr/lib/x86_64-linux-gnu:$ROOT/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QML2_IMPORT_PATH="$ROOT/usr/lib/x86_64-linux-gnu/qt5/qml${QML2_IMPORT_PATH:+:$QML2_IMPORT_PATH}"
export QML_IMPORT_PATH="$QML2_IMPORT_PATH"
export QT_PLUGIN_PATH="$ROOT/usr/lib/x86_64-linux-gnu/qt5/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
export GST_PLUGIN_PATH="$ROOT/usr/lib/x86_64-linux-gnu/gstreamer-1.0${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
export DISPLAY="${DISPLAY:-:0}"
# 视频后端：x86 预编译包内置 avcodec(custom_rtp) + qmlglsink 两条路径
export QOPENHD_ENABLE_GSTREAMER_QMLGLSINK=1
exec "$ROOT/usr/local/bin/QOpenHD" "$@"