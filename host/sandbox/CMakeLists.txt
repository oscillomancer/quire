supple_add_library(sandbox
  SOURCES src/render.cpp src/session.cpp src/commands.cpp src/analysis_commands.cpp src/optimize.cpp
  DEPS supple::clap-host supple::audio-file supple::core-json supple::midi
       supple::notation supple::command-bus supple::semantic-params-abi supple::core-analysis supple::optimize supple::core-params)

if(SUPPLE_BUILD_TESTS)
  supple_add_tests(sandbox SOURCES tests/test_render.cpp tests/test_session.cpp tests/test_commands.cpp LABELS rt)
  add_dependencies(sandbox.tests clap-host-fixtures)
  target_compile_definitions(sandbox.tests PRIVATE FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>")
  # The optimizer end to end with the real synth (slow-ish; single-threaded).
  add_executable(sandbox-optimize.tests tests/test_optimize.cpp)
  target_link_libraries(sandbox-optimize.tests PRIVATE supple::sandbox supple::doctest-main)
  supple_target_defaults(sandbox-optimize.tests)
  add_test(NAME sandbox-optimize.tests COMMAND sandbox-optimize.tests)
  set_tests_properties(sandbox-optimize.tests PROPERTIES LABELS "sandbox;slow;tsan-skip")
  add_dependencies(sandbox-optimize.tests orrery-headless clap-host-fixtures)
  target_compile_definitions(sandbox-optimize.tests PRIVATE
    ORRERY_CLAP_PATH="$<TARGET_FILE:orrery-headless>"
    FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>")
endif()

# The sandbox CLI: <slug>-sandbox (named from cmake/Product.cmake).
add_executable(${PRODUCT_SLUG}-sandbox tools/sandbox_main.cpp)
target_link_libraries(${PRODUCT_SLUG}-sandbox PRIVATE supple::sandbox supple::mcp supple::product-info)
supple_target_defaults(${PRODUCT_SLUG}-sandbox)

if(SUPPLE_BUILD_TESTS)
  # Drift test: the committed generator output must match the registry.
  add_test(NAME sandbox.registry-drift
    COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:${PRODUCT_SLUG}-sandbox>
            -DGENERATED=${PROJECT_SOURCE_DIR}/skill/generated
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_generated.cmake)
endif()

if(SUPPLE_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME sandbox.mcp-stdio
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/mcp_client.py
            $<TARGET_FILE:${PRODUCT_SLUG}-sandbox> session_get)
  add_test(NAME sandbox.mcp-proxy
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/mcp_proxy.py
            $<TARGET_FILE:${PRODUCT_SLUG}-sandbox> $<TARGET_FILE:${PRODUCT_SLUG}-mcp>
            $<TARGET_FILE:orrery-headless>)
  # The full real-synth session runs in the dev and GCC debug presets only.
  if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT SUPPLE_SANITIZE)
    add_test(NAME mcp.demo
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/mcp_demo.py
              $<TARGET_FILE:${PRODUCT_SLUG}-sandbox> $<TARGET_FILE:${PRODUCT_SLUG}-mcp>
              $<TARGET_FILE:orrery-headless>
              ${PROJECT_SOURCE_DIR}/docs/demos/mcp-orrery-session.md)
    set_tests_properties(mcp.demo PROPERTIES LABELS "sandbox;slow")
  endif()
  add_dependencies(${PRODUCT_SLUG}-sandbox orrery-headless)
endif()
