include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Identity.cmake)
supple_add_library(comp-engine
  SOURCES src/params.cpp src/processor.cpp
  DEPS supple::core-base supple::core-params supple::core-patch supple::core-dsp)
target_compile_definitions(comp-engine PUBLIC SUPPLE_COMP_DISPLAY_NAME="${COMP_DISPLAY_NAME}")
supple_add_tests(comp-engine SOURCES tests/test_comp.cpp DEPS supple::core-analysis LABELS rt)
