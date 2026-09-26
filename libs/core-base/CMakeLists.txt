supple_add_library(core-base
  SOURCES src/result.cpp
  DEPS supple::product-info)

supple_add_tests(core-base SOURCES tests/test_result.cpp)
