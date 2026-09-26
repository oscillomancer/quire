supple_add_library(core-patch
  SOURCES src/patch.cpp
  DEPS supple::core-base supple::core-json supple::core-schema supple::core-params)

supple_add_tests(core-patch SOURCES tests/test_patch.cpp)
