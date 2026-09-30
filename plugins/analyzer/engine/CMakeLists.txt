include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Identity.cmake")
supple_add_library(analyzer-engine
  SOURCES src/params.cpp src/processor.cpp
  DEPS supple::core-base supple::core-params supple::core-patch supple::core-analysis)
target_compile_definitions(analyzer-engine PUBLIC SUPPLE_ANALYZER_DISPLAY_NAME="${ANALYZER_DISPLAY_NAME}")
supple_add_tests(analyzer-engine SOURCES tests/test_engine.cpp LABELS rt)
