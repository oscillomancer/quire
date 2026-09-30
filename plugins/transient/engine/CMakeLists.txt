include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Identity.cmake)
supple_add_library(transient-engine
  SOURCES src/params.cpp src/processor.cpp
  DEPS supple::core-base supple::core-params supple::core-patch supple::core-dsp)
target_compile_definitions(transient-engine PUBLIC SUPPLE_TRANSIENT_DISPLAY_NAME="${TRANSIENT_DISPLAY_NAME}")
supple_add_tests(transient-engine SOURCES tests/test_transient.cpp LABELS rt)
