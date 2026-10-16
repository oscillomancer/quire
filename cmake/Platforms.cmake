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
  # Windows 10 1903 (UTF-8 process code page); no configuration changes on the build machine.
  add_compile_definitions(_USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN= _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
endif()

# Every executable target under `dir`, recursively, gets the UTF-8 code-page
# manifest (ADR-0044). Called once at the end of the root CMakeLists.
function(supple_windows_utf8_manifests dir)
  get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(type ${target} TYPE)
    get_target_property(imported ${target} IMPORTED)
    if(type STREQUAL "EXECUTABLE" AND NOT imported)
      target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/cmake/windows/utf8.manifest")
    endif()
  endforeach()
  get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(subdir IN LISTS subdirs)
    supple_windows_utf8_manifests("${subdir}")
  endforeach()
endfunction()
