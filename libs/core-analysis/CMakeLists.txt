supple_add_library(core-analysis SOURCES src/loudness.cpp DEPS supple::core-base supple::core-dsp)
supple_add_tests(core-analysis SOURCES tests/test_loudness.cpp LABELS rt)
