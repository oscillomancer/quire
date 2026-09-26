supple_add_library(model
  SOURCES src/session.cpp src/schemas.cpp src/versions.cpp
  DEPS supple::core-base supple::core-json supple::core-schema supple::midi supple::timeline)
if(SUPPLE_BUILD_TESTS)
  supple_add_tests(model SOURCES tests/test_model.cpp tests/test_versions.cpp)
endif()
