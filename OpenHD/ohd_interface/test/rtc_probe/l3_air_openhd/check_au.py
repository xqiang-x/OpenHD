#!/usr/bin/env python3
"""Annex-B AU 统计：SPS/PPS/IDR/P 计数、IDR 间隔（帧）、首 IDR 位置。
用法: check_au.py <file.h264>
"""
import sys

SPS, PPS, IDR, NONIDR = 7, 8, 5, 1

def main() -> int:
    data = open(sys.argv[1], "rb").read()
    nals = []  # (offset, nal_type)
    i, n = 0, len(data)
    starts = []
    while i < n - 4:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            starts.append((i + 3, data[i + 3] & 0x1F))
            i += 3
        elif i < n - 5 and data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 0 and data[i + 3] == 1:
            starts.append((i + 4, data[i + 4] & 0x1F))
            i += 4
        else:
            i += 1
    types = [t for _, t in starts]
    sps = types.count(SPS)
    pps = types.count(PPS)
    idr = types.count(IDR)
    p = types.count(NONIDR)
    print(f"nals total={len(types)} SPS={sps} PPS={pps} IDR={idr} P={p}")
    # v4l2h264enc 单 slice 输出：每帧 = 1 个条带 NAL（IDR 或 P），SPS/PPS 不占帧位
    frames = [t for t in types if t in (IDR, NONIDR)]
    idr_positions = [i for i, t in enumerate(frames) if t == IDR]
    gaps = [b - a for a, b in zip(idr_positions, idr_positions[1:])]
    print(f"AU≈{len(frames)}  IDR count={len(idr_positions)}  first IDR@{idr_positions[0] if idr_positions else '-'}")
    if gaps:
        print(f"IDR 间隔(帧): min={min(gaps)} max={max(gaps)} avg={sum(gaps)/len(gaps):.1f}")
    return 0

if __name__ == "__main__":
    sys.exit(main())