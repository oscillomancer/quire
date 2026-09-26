supple_add_library(core-schema SOURCES src/schema.cpp DEPS supple::core-json)
supple_add_tests(core-schema SOURCES tests/test_schema.cpp)
