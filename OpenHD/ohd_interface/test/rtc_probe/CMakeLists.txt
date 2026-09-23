cmake_minimum_required(VERSION 3.14)
project(rtc_probe LANGUAGES CXX)

# L1 本机全链仿真探针（rtc_air_probe / rtc_gnd_probe）
# 独立迷你构建，不进 OpenHD 主构建：
#   cmake -S <此目录> -B /tmp/rtc_probe_build && cmake --build /tmp/rtc_probe_build -j
# 视频路径复用 OpenHD 组件源码（rtp_es_assembler / es_rtp_packetizer，同一份文件零复制）

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

# tj_rtc 仓库根：由本目录回退 5 级（.../tj_rtc/OpenHD/OpenHD/ohd_interface/test/rtc_probe）
get_filename_component(_DEFAULT_TJ_RTC_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(TJ_RTC_ROOT "${_DEFAULT_TJ_RTC_ROOT}" CACHE PATH "tj_rtc repository root")

if(NOT EXISTS "${TJ_RTC_ROOT}/client_sdk/rtc_client_sdk/CMakeLists.txt")
    message(FATAL_ERROR "TJ_RTC_ROOT 不正确: ${TJ_RTC_ROOT}（应指向 tj_rtc 仓库根，可用 -DTJ_RTC_ROOT= 覆盖）")
endif()

if(NOT TARGET rtc_client_sdk)
    add_subdirectory(${TJ_RTC_ROOT}/client_sdk/rtc_client_sdk
                     ${CMAKE_BINARY_DIR}/_deps/rtc_client_sdk)
endif()

# OpenHD 组件源码与头文件（与主构建共用）
get_filename_component(OHD_INTERFACE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(ES_COMPONENT_SRC
    ${OHD_INTERFACE_DIR}/src/rtp_es_assembler.cpp
    ${OHD_INTERFACE_DIR}/src/es_rtp_packetizer.cpp
)

function(add_rtc_probe name)
    add_executable(${name} ${name}.cpp ${ES_COMPONENT_SRC})
    target_include_directories(${name} PRIVATE ${OHD_INTERFACE_DIR}/inc)
    target_link_libraries(${name} PRIVATE rtc_client_sdk Threads::Threads)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
endfunction()

add_rtc_probe(rtc_air_probe)
add_rtc_probe(rtc_gnd_probe)