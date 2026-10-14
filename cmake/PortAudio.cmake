# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# DAW-only static device library. Never built on Linux or in isolated plugins.
if(NOT APPLE AND NOT WIN32)
  return()
endif()
set(_pa ${PROJECT_SOURCE_DIR}/third_party/portaudio-19.7.0)
set(_pa_sources)
foreach(_source allocation converters cpuload debugprint dither front process ringbuffer stream trace)
  list(APPEND _pa_sources ${_pa}/src/common/pa_${_source}.c)
endforeach()
add_library(tp-portaudio STATIC ${_pa_sources})
target_include_directories(tp-portaudio SYSTEM PUBLIC ${_pa}/include)
target_include_directories(tp-portaudio PRIVATE ${_pa}/src/common)
target_compile_definitions(tp-portaudio PRIVATE PA_LITTLE_ENDIAN)
if(APPLE)
  target_sources(tp-portaudio PRIVATE ${_pa}/src/os/unix/pa_unix_hostapis.c
    ${_pa}/src/os/unix/pa_unix_util.c ${_pa}/src/hostapi/coreaudio/pa_mac_core.c
    ${_pa}/src/hostapi/coreaudio/pa_mac_core_blocking.c
    ${_pa}/src/hostapi/coreaudio/pa_mac_core_utilities.c)
  target_include_directories(tp-portaudio PRIVATE ${_pa}/src/os/unix)
  target_compile_definitions(tp-portaudio PRIVATE PA_USE_COREAUDIO=1 PA_USE_JACK=0 PA_USE_ALSA=0 PA_USE_OSS=0)
  target_link_libraries(tp-portaudio PRIVATE "-framework CoreAudio" "-framework AudioToolbox"
    "-framework AudioUnit" "-framework CoreFoundation" "-framework CoreServices")
  target_compile_options(tp-portaudio PRIVATE -w)
else()
  set(_asio ${PROJECT_SOURCE_DIR}/third_party/asiosdk-2.3.4)
  target_sources(tp-portaudio PRIVATE ${_pa}/src/os/win/pa_win_hostapis.c
    ${_pa}/src/os/win/pa_win_util.c ${_pa}/src/os/win/pa_win_waveformat.c
    ${_pa}/src/os/win/pa_win_wdmks_utils.c ${_pa}/src/os/win/pa_win_coinitialize.c
    ${_pa}/src/os/win/pa_x86_plain_converters.c
    ${_pa}/src/hostapi/wasapi/pa_win_wasapi.c
    ${_pa}/src/hostapi/asio/pa_asio.cpp ${_pa}/src/hostapi/asio/iasiothiscallresolver.cpp
    ${_asio}/common/asio.cpp ${_asio}/host/pc/asiolist.cpp ${_asio}/host/asiodrivers.cpp)
  target_include_directories(tp-portaudio PRIVATE ${_pa}/src/os/win ${_asio}/common ${_asio}/host ${_asio}/host/pc)
  target_compile_definitions(tp-portaudio PRIVATE PA_USE_WASAPI=1 PA_USE_ASIO=1
    PA_USE_DS=0 PA_USE_WMME=0 PA_USE_WDMKS=0)
  target_compile_options(tp-portaudio PRIVATE /w "/FI${CMAKE_CURRENT_LIST_DIR}/portaudio_windows.h")
  target_link_libraries(tp-portaudio PRIVATE winmm ole32 uuid avrt)
endif()
set_target_properties(tp-portaudio PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(supple::portaudio ALIAS tp-portaudio)
