include("${CMAKE_CURRENT_SOURCE_DIR}/../identity.cmake")
supple_add_library(eq-engine SOURCES src/params.cpp src/processor.cpp src/linear.cpp
  DEPS supple::core-base supple::core-params supple::core-patch supple::core-dsp)
supple_add_tests(eq-engine SOURCES tests/test_eq.cpp LABELS rt)
target_include_directories(eq-engine PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(SUPPLE_BUILD_TESTS)
  add_executable(eq-bench bench/eq_bench.cpp)
  target_link_libraries(eq-bench PRIVATE supple::eq-engine)
  supple_target_defaults(eq-bench)
endif()
