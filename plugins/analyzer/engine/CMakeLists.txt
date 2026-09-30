supple_add_library(analyzer-engine
  SOURCES src/params.cpp src/processor.cpp
  DEPS supple::core-base supple::core-params supple::core-patch supple::core-analysis)
supple_add_tests(analyzer-engine SOURCES tests/test_engine.cpp LABELS rt)
