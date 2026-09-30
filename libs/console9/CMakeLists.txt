supple_add_library(console9 SOURCES src/console9.cpp DEPS supple::core-base)

if(SUPPLE_BUILD_TESTS)
  set(reference "${PROJECT_SOURCE_DIR}/third_party/airwindows-console9")
  add_library(console9-reference STATIC
    "${reference}/Console9Channel/Console9Channel.cpp"
    "${reference}/Console9Channel/Console9ChannelProc.cpp"
    "${reference}/Console9Buss/Console9Buss.cpp"
    "${reference}/Console9Buss/Console9BussProc.cpp")
  target_include_directories(console9-reference PRIVATE tests/reference)
  target_compile_options(console9-reference PRIVATE -w -ffp-contract=off ${_supple_sanitize_flags})
  # Factories otherwise have the same symbol; leave upstream files untouched.
  set_source_files_properties("${reference}/Console9Channel/Console9Channel.cpp"
    PROPERTIES COMPILE_DEFINITIONS "createEffectInstance=createConsole9ChannelReference")
  set_source_files_properties("${reference}/Console9Buss/Console9Buss.cpp"
    PROPERTIES COMPILE_DEFINITIONS "createEffectInstance=createConsole9BussReference")
  supple_add_tests(console9 SOURCES tests/test_console9.cpp
    tests/reference/channel.cpp tests/reference/buss.cpp DEPS console9-reference LABELS rt)
  target_include_directories(console9.tests PRIVATE tests/reference)
  target_include_directories(console9.tests SYSTEM PRIVATE "${reference}")
endif()

add_executable(console9-bench bench/console9_bench.cpp)
target_link_libraries(console9-bench PRIVATE supple::console9)
supple_target_defaults(console9-bench)
