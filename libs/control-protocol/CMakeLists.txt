supple_add_library(control-protocol SOURCES src/protocol.cpp
  DEPS supple::command-bus supple::product-info)
supple_add_tests(control-protocol SOURCES tests/test_protocol.cpp)
