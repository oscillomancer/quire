supple_add_library(midi SOURCES src/midi.cpp src/smf.cpp DEPS supple::core-base)
supple_add_tests(midi SOURCES tests/test_midi.cpp)
