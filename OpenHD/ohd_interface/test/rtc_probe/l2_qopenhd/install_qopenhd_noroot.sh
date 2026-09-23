#!/bin/bash
# L2 出画验证：无 root 本地安装 QOpenHD（开发机/任意 Ubuntu 22.04 地面端）
# 官方 QOpenHD 的 Linux 分发在 OpenHD cloudsmith apt 仓库（jammy/amd64 deb），
# 本脚本用 apt-get download + dpkg-deb -x 装到本地前缀，不改系统（无需 sudo）。
# 用法：
#   ./install_qopenhd_noroot.sh [前缀目录，默认 /tmp/qopenhd_local]
set -e
PREFIX="${1:-/tmp/qopenhd_local}"
ROOT="$PREFIX/root"
DEBS="$PREFIX/debs"
QOPENHD_DEB_URL="https://dl.cloudsmith.io/public/openhd/release/deb/ubuntu/pool/jammy/main/q/qo/qopenhd_2.6.4-05-22-2025--16-01-09-3d325016/qopenhd_2.6.4-05-22-2025--16-01-09-3d325016_amd64.deb"

# QOpenHD 运行闭包（Ubuntu jammy 包名；缺失项逐一下载解包）
PKGS="libqt5texttospeech5 libqt5quickcontrols2-5 libqt5quicktemplates2-5
libqt5charts5 libqt5location5 libqt5location5-plugins libqt5positioning5
libqt5positioning5-plugins libqt5positioningquick5 libqt5quickparticles5
libqt5quickshapes5 libqt5webchannel5 libqt5webkit5 libqt5multimedia5
libqt5multimedia5-plugins libqt5multimediagsttools5 libqt5multimediawidgets5
libqt5multimediaquick5 libqt5sensors5 libqt5x11extras5 libqt5sql5-sqlite
libqt5help5 libqt5quicktest5 libqt5opengl5 libqt5waylandclient5
libqt5gstreamer-1.0-0 libqt5gstreamerquick-1.0-0 qtgstreamer-plugins-qt5
qml-module-qtquick-controls2 qml-module-qtquick-templates2
qml-module-qtquick-layouts qml-module-qtquick-window2 qml-module-qtquick-dialogs
qml-module-qtquick-extras qml-module-qtgraphicaleffects qml-module-qt-labs-platform
qml-module-qt-labs-settings qml-module-qtcharts qml-module-qtlocation
qml-module-qtpositioning qml-module-qtquick-controls qml-module-qtquick-shapes
qml-module-qtquick-particles2 qml-module-qtquick-privatewidgets
qml-module-qtmultimedia qml-module-qtsensors
gstreamer1.0-qt5"

mkdir -p "$ROOT" "$DEBS"
cd "$DEBS"

echo "== 下载 qopenhd deb =="
[ -f qopenhd.deb ] || curl -sL -o qopenhd.deb "$QOPENHD_DEB_URL"
dpkg-deb -x qopenhd.deb "$ROOT"

echo "== 下载/解包依赖（已装到系统的跳过）=="
for p in $PKGS; do
  if dpkg -s "$p" >/dev/null 2>&1; then continue; fi
  apt-get download "$p" >/dev/null 2>&1 || { echo "UNAVAILABLE $p（如为 cloudsmith 专用包需手动补）"; continue; }
  dpkg-deb -x "$p"_*.deb "$ROOT" 2>/dev/null || true
done

echo "== 依赖闭包检查（应无 not found）=="
export LD_LIBRARY_PATH="$ROOT/usr/lib/x86_64-linux-gnu:$ROOT/usr/lib"
{ ldd "$ROOT/usr/local/bin/QOpenHD"; find "$ROOT/usr/lib/x86_64-linux-gnu/qt5" -name '*.so*' -type f -exec ldd {} \; ; } 2>/dev/null | grep "not found" | sort -u || true

echo "== 完成：QOpenHD 位于 $ROOT/usr/local/bin/QOpenHD =="
echo "   启动： $(dirname "$0")/run_qopenhd.sh（前缀可用 QOPENHD_PREFIX 覆盖）"