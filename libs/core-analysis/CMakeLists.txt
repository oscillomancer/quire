supple_add_library(core-analysis SOURCES src/loudness.cpp DEPS supple::core-base supple::core-dsp)
# Single-threaded signal suites: ThreadSanitizer adds nothing but ~7 minutes.
supple_add_tests(core-analysis SOURCES tests/test_loudness.cpp LABELS rt tsan-skip)
