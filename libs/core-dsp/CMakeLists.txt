supple_add_library(core-dsp SOURCES src/dsp.cpp DEPS supple::core-base PRIVATE_DEPS supple::pocketfft)
supple_add_tests(core-dsp SOURCES tests/test_dsp.cpp LABELS rt)
