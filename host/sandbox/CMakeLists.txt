supple_add_library(sandbox
  SOURCES src/render.cpp src/session.cpp src/commands.cpp
  DEPS supple::clap-host supple::audio-file supple::core-json supple::midi
       supple::notation supple::command-bus supple::semantic-params-abi)

if(SUPPLE_BUILD_TESTS)
  supple_add_tests(sandbox SOURCES tests/test_render.cpp tests/test_session.cpp tests/test_commands.cpp LABELS rt)
  add_dependencies(sandbox.tests clap-host-fixtures)
  target_compile_definitions(sandbox.tests PRIVATE FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>")
endif()

# The sandbox CLI: <slug>-sandbox (named from cmake/Product.cmake).
add_executable(${PRODUCT_SLUG}-sandbox tools/sandbox_main.cpp)
target_link_libraries(${PRODUCT_SLUG}-sandbox PRIVATE supple::sandbox supple::product-info)
supple_target_defaults(${PRODUCT_SLUG}-sandbox)

if(SUPPLE_BUILD_TESTS)
  # Drift test: the committed generator output must match the registry.
  add_test(NAME sandbox.registry-drift
    COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:${PRODUCT_SLUG}-sandbox>
            -DGENERATED=${PROJECT_SOURCE_DIR}/skill/generated
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_generated.cmake)
endif()
