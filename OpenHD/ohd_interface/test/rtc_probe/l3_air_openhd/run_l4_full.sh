#!/bin/bash
# M5/L4 全链端到端（pi4 OpenHD air ↔ VPS SFU ↔ 开发机 rtc_gnd_probe）
# 覆盖四条通道：
#   视频：air 相机 → RtcLink → SFU → probe AU 落盘 → ffmpeg 全量解码 + NAL 统计
#   遥测：air 心跳(2Hz, MAVLink msgid=0) → ctrl 下行 → probe 逐帧解析计数
#   RC  ：probe MAVLink RC(msgid=70)@--ctrl-hz + 心跳 ~1Hz → ctrl 上行 → air 收包
#   音频：air audiotestsrc(PCMA) → RtcLink PushAudioFrame → probe 0x10 计数
# air 侧 enable_audio 由本脚本临时置 100（audiotestsrc），结束自动还原。
# 用法：./run_l4_full.sh                 # 60s；QOPENHD=1 出画
#       SECONDS_RUN=40 QOPENHD=0 ./run_l4_full.sh
# 前置：pi4 已按 run_ohd_air.sh 头注释部署并构建（含 M5 的 rtc_link 与 P1）
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
PI4="${PI4:-pi4}"
SERVER="${SERVER:-106.15.177.248}"
STREAM="${STREAM:-openhd0}"
WORK="${WORK:-/tmp/l4}"
SECONDS_RUN="${SECONDS_RUN:-60}"
CTRL_HZ="${CTRL_HZ:-20}"
QOPENHD="${QOPENHD:-1}"
PROBE_BIN="${PROBE_BIN:-/tmp/rtc_probe_build/rtc_gnd_probe}"
AIR_JSON=/usr/local/share/openhd/video/air_camera_generic.json
AIR_LOG=/tmp/l4_air.log
mkdir -p "$WORK"
rm -f "$WORK"/gnd.log "$WORK"/openhd_au.h264 "$WORK"/decode_err.log

restore_air_json() {
  if ssh "$PI4" "sudo test -f /tmp/air_camera_generic.l4bak"; then
    ssh "$PI4" "sudo cp /tmp/air_camera_generic.l4bak $AIR_JSON && \
        sudo rm -f /tmp/air_camera_generic.l4bak && echo 'air json 已还原'"
  fi
}
trap 'restore_air_json' EXIT

echo "== t=0: pi4 备份 air json 并置 enable_audio=100（audiotestsrc） =="
ssh "$PI4" "sudo cp -n $AIR_JSON /tmp/air_camera_generic.l4bak; \
    sudo python3 - '$AIR_JSON' <<'EOF'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
d['enable_audio'] = 100
json.dump(d, open(p, 'w'), indent=2)
EOF
    grep enable_audio $AIR_JSON"

AIR_SECS=$((SECONDS_RUN + 45))
echo "== t=1: pi4 启动 OpenHD air（${AIR_SECS}s，日志 $AIR_LOG） =="
# 注意：远端 ssh 会等到远端命令的所有 fd 关闭才返回；必须把 ssh 本身放本地后台，
# 否则脚本会阻塞到 air 退出（L4 首轮实测踩坑）。
ssh "$PI4" "cd \$HOME && sudo ./run_ohd_air.sh -t $AIR_SECS \
    > $AIR_LOG 2>&1 < /dev/null" &
AIR_SSH_PID=$!

echo "== 等待 air 接入 SFU（最多 45s） =="
for i in $(seq 1 45); do
  if ssh "$PI4" "sudo grep -q '已接入 SFU' $AIR_LOG 2>/dev/null"; then
    echo "air 已在 SFU 注册（等待 ${i}s）"
    break
  fi
  sleep 1
done
sleep 5  # 让首个媒体帧落地后再订阅

if [ "$QOPENHD" = "1" ]; then
  echo "== 启动 QOpenHD（probe RTP 5600 出画） =="
  "$DIR/../l2_qopenhd/run_qopenhd.sh" > "$WORK/qopenhd.log" 2>&1 &
  echo "QOpenHD pid $!"
fi

echo "== t=10: rtc_gnd_probe（${SECONDS_RUN}s，ctrl_hz=${CTRL_HZ}，AU 落盘） =="
"$PROBE_BIN" --server "$SERVER" --port 9200 --stream "$STREAM" \
    --user ohd_gnd_probe_l4 --out-ip 127.0.0.1 --out-port 5600 \
    --dump-au "$WORK/openhd_au.h264" --ctrl-hz "$CTRL_HZ" \
    --seconds "$SECONDS_RUN" > "$WORK/gnd.log" 2>&1
echo "probe exit=$?"
wait "$AIR_SSH_PID" 2>/dev/null || true  # air 已按 -t 自行退出

echo "== air 侧关键日志（ctrl 收发 / 音频接入 / 推流状态） =="
ssh "$PI4" "sudo grep -E '遥测|RC|音频|已接入 SFU|断连' $AIR_LOG | tail -15"

echo "== 视频：ffmpeg 全量解码 =="
ffmpeg -v error -i "$WORK/openhd_au.h264" -f null - 2> "$WORK/decode_err.log" \
    && echo "decode OK" || echo "decode FAILED"
wc -l < "$WORK/decode_err.log" | xargs echo "decode error lines:"
python3 "$DIR/check_au.py" "$WORK/openhd_au.h264" | head -12

echo "== ground probe 计数（结尾行） =="
grep -E "^\[gnd\] (结束|1s)" "$WORK/gnd.log" | tail -3