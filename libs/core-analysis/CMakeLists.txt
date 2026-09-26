supple_add_library(core-analysis
  SOURCES src/loudness.cpp src/analysis.cpp src/compare.cpp
  DEPS supple::core-base supple::core-dsp supple::core-json)
# Single-threaded signal suites: ThreadSanitizer adds nothing but minutes.
supple_add_tests(core-analysis SOURCES tests/test_loudness.cpp tests/test_analysis.cpp LABELS rt tsan-skip)
if(SUPPLE_BUILD_TESTS)
  target_compile_definitions(core-analysis.tests PRIVATE SUPPLE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()
