# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# Temporary port boundaries. Remove each gate when its platform implementation
# and tests land; keep Linux presets and target coverage unchanged.
set(SUPPLE_PLATFORM_LINUX OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(SUPPLE_PLATFORM_LINUX ON)
endif()
# JACK, X11 and their dependent services. Hosting, scanning and the bridge are portable.
set(SUPPLE_PLUGIN_HOST ON)
# Core-base waits/wakes, control-protocol and MCP are portable and ungated.
set(SUPPLE_HOST_SERVICES ${SUPPLE_PLATFORM_LINUX})
if(NOT SUPPLE_HOST_SERVICES)
  message(STATUS "Port gates: JACK (see CONTRIBUTING.md)")
  if(SUPPLE_BUILD_SPIKES OR SUPPLE_BUILD_FUZZERS)
    message(FATAL_ERROR "Research spikes and host fuzzers require Linux platform services")
  endif()
endif()
if(WIN32)
  # Windows SDK GL headers stop at 1.1. Legacy template-derived stock GUI
  # backends need these standard enums; keep their shared stack untouched.
  add_compile_definitions(GL_GENERATE_MIPMAP=0x8191 GL_CLAMP_TO_EDGE=0x812F
                         GL_INCR_WRAP=0x8507 GL_DECR_WRAP=0x8508)
  # Windows 10 1803; no configuration changes on the build machine.
  add_compile_definitions(_USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN= _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000005)
endif()
