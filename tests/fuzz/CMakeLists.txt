add_library(core-json-fuzzlib STATIC
  ${PROJECT_SOURCE_DIR}/libs/core-json/src/value.cpp
  ${PROJECT_SOURCE_DIR}/libs/core-json/src/parse.cpp
  ${PROJECT_SOURCE_DIR}/libs/core-json/src/write.cpp
  ${PROJECT_SOURCE_DIR}/libs/core-json/src/pointer.cpp
  ${PROJECT_SOURCE_DIR}/libs/core-json/src/merge.cpp)
target_include_directories(core-json-fuzzlib PUBLIC ${PROJECT_SOURCE_DIR}/libs/core-json/include)
target_link_libraries(core-json-fuzzlib PUBLIC supple::core-base)
target_compile_options(core-json-fuzzlib PRIVATE -fsanitize=fuzzer-no-link,address)
supple_target_defaults(core-json-fuzzlib)
add_executable(core-json.fuzz core_json_fuzz.cpp)
target_link_libraries(core-json.fuzz PRIVATE core-json-fuzzlib)
target_compile_options(core-json.fuzz PRIVATE -fsanitize=fuzzer,address)
target_link_options(core-json.fuzz PRIVATE -fsanitize=fuzzer,address)
supple_target_defaults(core-json.fuzz)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/corpus DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME core-json.fuzz-smoke COMMAND core-json.fuzz -runs=100 ${CMAKE_CURRENT_BINARY_DIR}/corpus)
set_tests_properties(core-json.fuzz-smoke PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

add_library(command-bus-fuzzlib STATIC
  ${PROJECT_SOURCE_DIR}/libs/command-bus/src/bus.cpp
  ${PROJECT_SOURCE_DIR}/libs/command-bus/src/jobs.cpp)
target_include_directories(command-bus-fuzzlib PUBLIC ${PROJECT_SOURCE_DIR}/libs/command-bus/include)
target_link_libraries(command-bus-fuzzlib PUBLIC supple::core-schema supple::product-info)
target_compile_options(command-bus-fuzzlib PRIVATE -fsanitize=fuzzer-no-link,address)
supple_target_defaults(command-bus-fuzzlib)
add_executable(command-bus.fuzz command_bus_fuzz.cpp)
target_link_libraries(command-bus.fuzz PRIVATE command-bus-fuzzlib)
target_compile_options(command-bus.fuzz PRIVATE -fsanitize=fuzzer,address)
target_link_options(command-bus.fuzz PRIVATE -fsanitize=fuzzer,address)
supple_target_defaults(command-bus.fuzz)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/bus-corpus DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME command-bus.fuzz-smoke
  COMMAND command-bus.fuzz -runs=100 ${CMAKE_CURRENT_BINARY_DIR}/bus-corpus)
set_tests_properties(command-bus.fuzz-smoke PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
