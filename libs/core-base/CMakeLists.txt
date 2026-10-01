supple_add_library(core-base
  SOURCES src/result.cpp src/units.cpp src/closest.cpp src/files.cpp src/sha256.cpp
  DEPS supple::product-info)

supple_add_tests(core-base SOURCES tests/test_result.cpp tests/test_rt_primitives.cpp tests/test_files.cpp tests/test_environment.cpp tests/test_sha256.cpp LABELS rt)
