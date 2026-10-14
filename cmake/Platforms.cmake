# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# Temporary port boundaries. Remove each gate when its platform implementation
# and tests land; keep Linux presets and target coverage unchanged.
set(SUPPLE_PLATFORM_LINUX OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(SUPPLE_PLATFORM_LINUX ON)
endif()
# Futex/eventfd, process isolation, JACK, X11 and the services depending on them.
set(SUPPLE_PLATFORM_SERVICES ${SUPPLE_PLATFORM_LINUX})
# Recording recovery and RF64 output still use POSIX file descriptors.
set(SUPPLE_POSIX_AUDIO_FILES OFF)
if(UNIX)
  set(SUPPLE_POSIX_AUDIO_FILES ON)
endif()
if(NOT SUPPLE_PLATFORM_SERVICES)
  message(STATUS "Port gates: scheduling/wake relay, hosting/bridge, engine/JACK, control/MCP, GUI/agent, plugin wrappers (see CONTRIBUTING.md)")
  set(SUPPLE_BUILD_GUI OFF CACHE BOOL "Build GUI components" FORCE)
  set(SUPPLE_BUILD_PLUGINS OFF CACHE BOOL "Build the stock plugins (template-derived)" FORCE)
  if(SUPPLE_BUILD_SPIKES OR SUPPLE_BUILD_FUZZERS)
    message(FATAL_ERROR "Research spikes and host fuzzers require Linux platform services")
  endif()
endif()
if(WIN32)
  # Windows 10 1803; no configuration changes on the build machine.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000005)
endif()
