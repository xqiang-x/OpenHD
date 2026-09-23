#!/bin/bash
# M2 冒烟运行（L1.5）：gst 源 → air_smoke(RtcLink air) → VPS SFU
#   ├→ gnd_smoke(RtcLink ground)：看护重订阅/统计/转发通道计数
#   └→ rtc_gnd_probe：AU 落盘 → 与 gst 源 Annex-B 逐帧 framemd5 对照
# 前置：
#   1) ./build.sh 已构建 air_smoke/gnd_smoke（默认产物在 SDK_BUILD=/tmp/rtc_probe_build，
#      可用 SMOKE_BIN_DIR 覆盖），probe 已构建（PROBE_BIN 可覆盖）
#   2) /tmp/rtc_smoke/rtc.txt（或 RTC_SMOKE_CONFIG_DIR）包含 server_ip 等配置，
#      参考同目录 rtc.txt.example；air/gnd 共用该文件（stream_prefix=ohdsmoke）
set -u
SMOKE_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SMOKE_DIR"
SMOKE_BIN_DIR="${SMOKE_BIN_DIR:-/tmp/rtc_probe_build}"
PROBE_BIN="${PROBE_BIN:-/tmp/rtc_probe_build/rtc_gnd_probe}"
SERVER="${SERVER:-106.15.177.248}"
STREAM="${STREAM:-ohdsmoke0}"
WORK="${WORK:-/tmp/rtc_smoke}"
GST_SRC="$WORK/l15_src.h264"
AU_DUMP="$WORK/l15_au.h264"
mkdir -p "$WORK"
rm -f "$GST_SRC" "$AU_DUMP" "$WORK"/l15_*.log

echo "== t=0: air(RtcLink) 35s + gnd(RtcLink) 40s =="
"$SMOKE_BIN_DIR/air_smoke" 15997 35 > "$WORK/l15_air.log" 2>&1 &
AIR_PID=$!
"$SMOKE_BIN_DIR/gnd_smoke" 40 > "$WORK/l15_gnd.log" 2>&1 &
GND_PID=$!

sleep 4
echo "== t=4: probe（28s，AU 落盘） =="
"$PROBE_BIN" --server "$SERVER" --port 9200 --stream "$STREAM" \
    --user ohd_gnd_probe_smoke --dump-au "$AU_DUMP" --seconds 28 \
    > "$WORK/l15_probe.log" 2>&1 &
PROBE_PID=$!

sleep 1
echo "== t=5: gst 源（28s） =="
timeout -s INT 28 gst-launch-1.0 -e videotestsrc is-live=true pattern=ball \
    ! video/x-raw,width=640,height=360,framerate=30/1 \
    ! x264enc tune=zerolatency speed-preset=ultrafast bitrate=1500 key-int-max=30 \
    ! h264parse config-interval=1 ! rtph264pay pt=96 mtu=1400 ! tee name=t \
    t. ! queue ! udpsink host=127.0.0.1 port=15997 \
    t. ! queue ! rtph264depay ! h264parse ! video/x-h264,stream-format=byte-stream \
    ! filesink location="$GST_SRC" > "$WORK/l15_gst.log" 2>&1

echo "== gst 结束，等待进程退出 =="
wait $AIR_PID; wait $PROBE_PID; wait $GND_PID

echo "== framemd5 对照（gst 源 vs RtcLink ground 收到的 AU） =="
ffmpeg -v error -i "$GST_SRC" -f framemd5 - 2>/dev/null | grep -v '^#' \
    | awk '{print $NF}' > "$WORK/l15_src.md5"
ffmpeg -v error -i "$AU_DUMP" -f framemd5 - 2>/dev/null | grep -v '^#' \
    | awk '{print $NF}' > "$WORK/l15_au.md5"
python3 - "$WORK/l15_src.md5" "$WORK/l15_au.md5" <<'EOF'
import sys
src = open(sys.argv[1]).read().split()
au  = open(sys.argv[2]).read().split()
print(f"src frames={len(src)} au frames={len(au)}")
off = src.index(au[0]) if au and au[0] in src else None
print("offset =", off)
if off is not None:
    win = src[off:off+len(au)]
    same = sum(1 for a, b in zip(win, au) if a == b)
    print(f"framemd5 compare: {same}/{len(au)} identical, mismatches={len(au)-same}")
EOF

echo "== 关键日志 =="
grep -E "已订阅|重建订阅|订阅失败|PLI|first upstream ctrl" "$WORK/l15_gnd.log" "$WORK/l15_air.log" \
    | grep -v "^--$" | head -20
echo "== 计数 =="
grep -E "done:" "$WORK/l15_air.log" "$WORK/l15_gnd.log" "$WORK/l15_probe.log"
echo "== M5 ctrl/audio 对照（期望：telem_rx≈air telem_tx；rc_rx 为 0——RC 只到 air；" \
     "air ctrl_rx≈4×air 运行期内 gnd rc_tx（n_injections=4）；audio_rx≈air audio_tx） =="
grep -h "done:" "$WORK/l15_air.log" | sed 's/^/[air]  /'
grep -h "done:" "$WORK/l15_gnd.log" | sed 's/^/[gnd]  /'