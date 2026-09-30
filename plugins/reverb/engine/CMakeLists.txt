include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Identity.cmake)
supple_add_library(reverb-engine SOURCES src/params.cpp src/processor.cpp
 DEPS supple::core-base supple::core-params supple::core-patch supple::core-dsp)
target_compile_definitions(reverb-engine PUBLIC SUPPLE_REVERB_DISPLAY_NAME="${REVERB_DISPLAY_NAME}")
supple_add_tests(reverb-engine SOURCES tests/test_reverb.cpp LABELS rt)
if(SUPPLE_BUILD_TESTS)
 # Keep the full numerical matrix under TSan; instrumentation makes 90 IRs slow.
 if(SUPPLE_SANITIZE STREQUAL "thread")
  set_tests_properties(reverb-engine.tests PROPERTIES TIMEOUT 1800)
 endif()
 add_executable(reverb-bench bench/reverb_bench.cpp)
 target_link_libraries(reverb-bench PRIVATE supple::reverb-engine)
 supple_target_defaults(reverb-bench)
endif()
