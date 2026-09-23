#!/bin/bash
# M4/L3 ground 侧（开发机）：订阅 OpenHD air 的 RTC 流 → AU 落盘 + RTP 转发
#   1) rtc_gnd_probe 订阅 openhd0：AU 落盘 /tmp/l3/openhd_au.h264，RTP → 127.0.0.1:5600
#   2) 可选 QOpenHD 出画（QOPENHD=1，需 l2_qopenhd 已装好）
#   3) ffmpeg 全量解码 + NAL 类型统计（SPS/PPS/IDR/P）
# 前置：air 侧 openhd 已在 pi4 运行（sudo ./run_ohd_air.sh -t N），stream=openhd0
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
PROBE_BIN="${PROBE_BIN:-/tmp/rtc_probe_build/rtc_gnd_probe}"
SERVER="${SERVER:-106.15.177.248}"
PORT="${PORT:-9200}"
STREAM="${STREAM:-openhd0}"
WORK="${WORK:-/tmp/l3}"
SECONDS_RUN="${SECONDS_RUN:-40}"
mkdir -p "$WORK"
rm -f "$WORK/openhd_au.h264" "$WORK/probe.log"

if [ "${QOPENHD:-0}" = "1" ]; then
    echo "== 启动 QOpenHD（RTP 5600 出画） =="
    "$DIR/../l2_qopenhd/run_qopenhd.sh" > "$WORK/qopenhd.log" 2>&1 &
    echo "QOpenHD pid $!"
    sleep 5
fi

echo "== rtc_gnd_probe 订阅 ${STREAM}（${SECONDS_RUN}s） =="
"$PROBE_BIN" --server "$SERVER" --port "$PORT" --stream "$STREAM" \
    --user ohd_gnd_probe_l3 --out-ip 127.0.0.1 --out-port 5600 \
    --dump-au "$WORK/openhd_au.h264" --seconds "$SECONDS_RUN" --verbose \
    > "$WORK/probe.log" 2>&1
echo "probe exit=$?"

echo "== ffmpeg 全量解码（错误将在此暴露） =="
ffmpeg -v error -i "$WORK/openhd_au.h264" -f null - 2> "$WORK/decode_err.log" \
    && echo "decode OK" || echo "decode FAILED"
wc -l < "$WORK/decode_err.log" | xargs echo "decode error lines:"

echo "== NAL 统计 =="
python3 "$DIR/check_au.py" "$WORK/openhd_au.h264" | head -30

echo "== probe 计数 =="
grep -E "done:|丢失|lost|rtt" "$WORK/probe.log" | tail -5