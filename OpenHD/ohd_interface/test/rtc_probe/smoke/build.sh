#!/bin/bash
# M2 冒烟构建（L1.5）：RtcLink 真实类 + OpenHD 公共源 + tj_rtc SDK 静态库
# 前置：先构建 SDK 静态库（test/rtc_probe/CMakeLists.txt 的 cmake 构建，默认产物
#       在 /tmp/rtc_probe_build；可用 SDK_BUILD=<dir> 覆盖）。
set -e
SMOKE_DIR="$(cd "$(dirname "$0")" && pwd)"
TJ_RTC_ROOT="$(cd "${SMOKE_DIR}/../../../../../.." && pwd)"
SDK_BUILD="${SDK_BUILD:-/tmp/rtc_probe_build}"
OUT_DIR="${OUT_DIR:-$SDK_BUILD}"   # 产物放构建目录，保持仓库干净

mkdir -p "$OUT_DIR"
INC="-I ${TJ_RTC_ROOT}/OpenHD/OpenHD/ohd_common/inc \
-I ${TJ_RTC_ROOT}/OpenHD/OpenHD/ohd_interface/inc \
-I ${TJ_RTC_ROOT}/OpenHD/OpenHD/ohd_common/lib/spdlog/include \
-I ${TJ_RTC_ROOT}/client_sdk/rtc_client_sdk/include \
-I ${TJ_RTC_ROOT}/common/sfu_protocol \
-I ${TJ_RTC_ROOT}/libs/mem_pool -I ${TJ_RTC_ROOT}/libs/frame_protoc/src"
SRC="${TJ_RTC_ROOT}/OpenHD/OpenHD/ohd_interface/src/rtc_link.cpp \
${TJ_RTC_ROOT}/OpenHD/OpenHD/ohd_interface/src/rtc_config.cpp \
${TJ_RTC_ROOT}/OpenHD/OpenHD/ohd_interface/src/rtp_es_assembler.cpp \
${TJ_RTC_ROOT}/OpenHD/OpenHD/ohd_interface/src/es_rtp_packetizer.cpp \
${TJ_RTC_ROOT}/OpenHD/OpenHD/ohd_common/src/openhd_spdlog.cpp \
${TJ_RTC_ROOT}/OpenHD/OpenHD/ohd_common/src/openhd_util.cpp \
${TJ_RTC_ROOT}/OpenHD/OpenHD/ohd_common/src/openhd_util_filesystem.cpp"
LIBS="${SDK_BUILD}/_deps/rtc_client_sdk/librtc_client_sdk.a \
${SDK_BUILD}/_deps/frame_protoc/libframe_protoc.a \
${SDK_BUILD}/_deps/pacer/libpacer_lib.a \
${SDK_BUILD}/_deps/mem_pool/libmem_pool_lib.a -lpthread"

# -include spdlog/spdlog.h 复刻 OHDCommonLib 的 target_precompile_headers
g++ -std=c++17 -O2 -Wall -Wextra -include spdlog/spdlog.h $INC \
    "${SMOKE_DIR}/air_smoke.cpp" $SRC "${SMOKE_DIR}/config_paths_stub.cpp" $LIBS \
    -o "${OUT_DIR}/air_smoke"
g++ -std=c++17 -O2 -Wall -Wextra -include spdlog/spdlog.h $INC \
    "${SMOKE_DIR}/gnd_smoke.cpp" $SRC "${SMOKE_DIR}/config_paths_stub.cpp" $LIBS \
    -o "${OUT_DIR}/gnd_smoke"
echo "build OK: ${OUT_DIR}/{air_smoke,gnd_smoke}"