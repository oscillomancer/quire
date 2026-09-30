supple_add_library(gate-engine SOURCES src/params.cpp src/processor.cpp DEPS supple::core-dsp supple::core-params supple::core-patch)
supple_add_tests(gate-engine SOURCES tests/test_engine.cpp LABELS rt)
