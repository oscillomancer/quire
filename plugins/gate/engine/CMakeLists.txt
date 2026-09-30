include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Identity.cmake")
supple_add_library(gate-engine SOURCES src/params.cpp src/processor.cpp DEPS supple::core-dsp supple::core-params supple::core-patch)
target_compile_definitions(gate-engine PUBLIC SUPPLE_GATE_DISPLAY_NAME="${GATE_DISPLAY_NAME}")
supple_add_tests(gate-engine SOURCES tests/test_engine.cpp LABELS rt)
