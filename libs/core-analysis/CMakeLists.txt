# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
supple_add_library(core-analysis
  SOURCES src/loudness.cpp src/analysis.cpp src/compare.cpp
  DEPS supple::core-base supple::core-dsp supple::core-json)
# Optimizer runs call this offline library hundreds of times in dev builds.
# Keep sanitizer builds unoptimized for diagnostics.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT SUPPLE_SANITIZE)
  target_compile_options(core-analysis PRIVATE -O2)
endif()
# Single-threaded signal suites: ThreadSanitizer adds nothing but minutes.
supple_add_tests(core-analysis SOURCES tests/test_loudness.cpp tests/test_analysis.cpp LABELS rt tsan-skip)
if(SUPPLE_BUILD_TESTS)
  target_compile_definitions(core-analysis.tests PRIVATE SUPPLE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  # A sanitized build of these suites is several times slower than a plain one:
  # the preset's 600 s default was reached at 612 s under UBSan on a loaded
  # machine, which reads as a failure. Give them room whenever ASan or UBSan is
  # on (the test's own TIMEOUT wins over the preset's default).
  if(SUPPLE_SANITIZE MATCHES "address|undefined")
    set_tests_properties(core-analysis.tests PROPERTIES TIMEOUT 1800)
  endif()
endif()
