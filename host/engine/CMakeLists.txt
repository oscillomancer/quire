add_subdirectory(model)

supple_add_library(engine
  SOURCES src/offline.cpp src/plan.cpp src/realtime.cpp src/app.cpp src/commands.cpp
  DEPS supple::model supple::clap-host supple::audio-file supple::timeline supple::semantic-params-abi
       supple::command-bus supple::notation)

if(SUPPLE_BUILD_TESTS)
  supple_add_tests(engine SOURCES tests/test_offline.cpp tests/test_realtime.cpp tests/test_app.cpp LABELS rt)
  add_dependencies(engine.tests clap-host-fixtures)
  target_compile_definitions(engine.tests PRIVATE FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>")
endif()

# The headless DAW CLI: <slug>-engine (named from cmake/Product.cmake).
add_executable(${PRODUCT_SLUG}-engine tools/engine_main.cpp)
target_link_libraries(${PRODUCT_SLUG}-engine PRIVATE supple::engine supple::mcp supple::product-info)
supple_target_defaults(${PRODUCT_SLUG}-engine)

if(SUPPLE_BUILD_TESTS)
  add_test(NAME engine.registry-drift
    COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:${PRODUCT_SLUG}-engine>
            -DGENERATED=${PROJECT_SOURCE_DIR}/skill/generated -DPREFIX=engine
            -P ${PROJECT_SOURCE_DIR}/host/sandbox/tools/check_generated.cmake)
endif()

if(SUPPLE_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME engine.mcp-stdio
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/mcp_client.py
            $<TARGET_FILE:${PRODUCT_SLUG}-engine> session_describe)
endif()
