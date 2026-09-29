supple_add_library(core-analysis
  SOURCES src/loudness.cpp src/analysis.cpp src/compare.cpp src/live.cpp
  DEPS supple::core-base supple::core-dsp supple::core-json)
# Optimizer runs call this offline library hundreds of times in dev builds.
# Keep sanitizer builds unoptimized for diagnostics.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT SUPPLE_SANITIZE)
  target_compile_options(core-analysis PRIVATE -O2)
endif()
# Single-threaded signal suites: ThreadSanitizer adds nothing but minutes.
supple_add_tests(core-analysis SOURCES tests/test_loudness.cpp tests/test_analysis.cpp tests/test_live.cpp LABELS rt tsan-skip)
if(SUPPLE_BUILD_TESTS)
  target_compile_definitions(core-analysis.tests PRIVATE SUPPLE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()
