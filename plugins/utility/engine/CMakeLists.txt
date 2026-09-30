include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Identity.cmake")
supple_add_library(utility-engine SOURCES src/params.cpp src/processor.cpp
  DEPS supple::core-base supple::core-params supple::core-patch supple::core-dsp)
target_compile_definitions(utility-engine PUBLIC SUPPLE_UTILITY_DISPLAY_NAME="${UTILITY_DISPLAY_NAME}")
supple_add_tests(utility-engine SOURCES tests/test_utility.cpp LABELS rt)
