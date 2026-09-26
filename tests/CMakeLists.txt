# Cross-library tests: rt-safety gate, integration, fuzz, golden renders.
if(SUPPLE_BUILD_TESTS AND SUPPLE_SANITIZE MATCHES "realtime")
  add_executable(rtsan-selfcheck rt-safety/rtsan_selfcheck.cpp)
  target_link_libraries(rtsan-selfcheck PRIVATE supple::core-base)
  supple_target_defaults(rtsan-selfcheck)
  target_compile_options(rtsan-selfcheck PRIVATE -Wno-function-effects)
  add_test(NAME rtsan-selfcheck COMMAND rtsan-selfcheck)
  set_tests_properties(rtsan-selfcheck PROPERTIES WILL_FAIL TRUE)
endif()
