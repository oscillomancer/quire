supple_add_library(core-base
  SOURCES src/result.cpp src/units.cpp src/closest.cpp
  DEPS supple::product-info)

supple_add_tests(core-base SOURCES tests/test_result.cpp tests/test_rt_primitives.cpp LABELS rt)
