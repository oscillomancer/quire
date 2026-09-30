include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Identity.cmake")
supple_add_library(delay-engine SOURCES src/params.cpp src/processor.cpp
  DEPS supple::core-dsp supple::core-params supple::core-patch)
target_compile_definitions(delay-engine PUBLIC SUPPLE_DELAY_DISPLAY_NAME="${DELAY_DISPLAY_NAME}")
supple_add_tests(delay-engine SOURCES tests/test_delay.cpp LABELS rt)
