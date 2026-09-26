supple_add_library(core-json SOURCES src/value.cpp src/parse.cpp src/write.cpp src/pointer.cpp src/merge.cpp DEPS supple::core-base)
supple_add_tests(core-json SOURCES tests/test_json.cpp)
