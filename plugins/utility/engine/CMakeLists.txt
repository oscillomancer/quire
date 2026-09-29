supple_add_library(utility-engine SOURCES src/params.cpp src/processor.cpp
  DEPS supple::core-base supple::core-params supple::core-patch supple::core-dsp)
supple_add_tests(utility-engine SOURCES tests/test_utility.cpp LABELS rt)
