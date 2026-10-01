add_subdirectory(model)

supple_add_library(engine
  SOURCES src/offline.cpp src/plan.cpp src/workers.cpp src/realtime.cpp src/recording.cpp
          src/stretch.cpp
          src/jack_backend.cpp
          src/app.cpp src/commands.cpp src/extra_commands.cpp src/midi_commands.cpp src/analysis_commands.cpp
          src/render_commands.cpp
  DEPS supple::model supple::clap-host supple::audio-file supple::timeline supple::semantic-params-abi
       supple::command-bus supple::lua-bus supple::notation supple::core-params supple::core-analysis
       supple::core-dsp supple::signalsmith-stretch supple::console9
  PRIVATE_DEPS supple::product-info ${CMAKE_DL_LIBS})

if(SUPPLE_BUILD_TESTS)
  # A stand-in libjack: the JACK backend is tested without a server or device.
  add_library(engine-fake-jack MODULE tests/fake_jack.cpp)
  set_target_properties(engine-fake-jack PROPERTIES PREFIX "" OUTPUT_NAME "fake-libjack")
  supple_target_defaults(engine-fake-jack)
  # Its handle types are its own (as a real libjack's, which is C and carries
  # no signature for -fsanitize=function to check).
  target_compile_options(engine-fake-jack PRIVATE $<$<CXX_COMPILER_ID:Clang>:-fno-sanitize=function>)

  supple_add_tests(engine SOURCES tests/test_offline.cpp tests/test_realtime.cpp tests/test_app.cpp
                   tests/test_commands.cpp tests/test_scripts.cpp tests/test_strips.cpp
                   tests/test_jack.cpp tests/test_workers.cpp LABELS rt)
  target_include_directories(engine.tests PRIVATE src)
  target_include_directories(engine.tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/fuzz)
  add_dependencies(engine.tests clap-host-fixtures clap-host-vst3-fixtures engine-fake-jack ${PRODUCT_SLUG}-bridge)
  target_compile_definitions(engine.tests PRIVATE FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>"
                             FIXTURE_VST3_PATH="${CMAKE_BINARY_DIR}/host/clap-host/test-vst3.vst3"
                             FAKE_JACK_PATH="$<TARGET_FILE:engine-fake-jack>"
                             BRIDGE_PATH="$<TARGET_FILE:${PRODUCT_SLUG}-bridge>")
  target_link_libraries(engine.tests PRIVATE ${CMAKE_DL_LIBS})
  add_test(NAME engine.strips-fuzz COMMAND engine.tests "-tc=engine.strips-fuzz*")
  set_tests_properties(engine.strips-fuzz PROPERTIES LABELS "engine;rt")
endif()

# Capacity benchmark (engine.md §7 B1), with a command-built shared session.
add_executable(engine-bench bench/engine_bench.cpp bench/session.cpp)
target_link_libraries(engine-bench PRIVATE supple::engine)
target_include_directories(engine-bench PRIVATE src)
supple_target_defaults(engine-bench)
target_compile_definitions(engine-bench PRIVATE
  BENCH_DRIVER="${PROJECT_SOURCE_DIR}/tools/bench/run.py"
  BENCH_RECIPE="${CMAKE_CURRENT_SOURCE_DIR}/bench/session.json"
  BENCH_PLUGINS="${CMAKE_BINARY_DIR}/plugins/clap")
if(SUPPLE_BUILD_TESTS)
  target_sources(engine.tests PRIVATE tests/test_bench.cpp bench/session.cpp)
  target_include_directories(engine.tests PRIVATE bench)
  target_compile_definitions(engine.tests PRIVATE BENCH_RECIPE="${CMAKE_CURRENT_SOURCE_DIR}/bench/session.json")
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME engine.bench-tooling COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${PROJECT_SOURCE_DIR}/tools/bench -p "test_*.py")
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
  add_test(NAME engine.mcp-script
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/mcp_script.py
            $<TARGET_FILE:${PRODUCT_SLUG}-engine> engine)
  add_test(NAME engine.mcp-proxy
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/mcp_proxy.py
            $<TARGET_FILE:${PRODUCT_SLUG}-engine> $<TARGET_FILE:${PRODUCT_SLUG}-mcp> --engine)
  # Phase 4 acceptance: an arrangement built and rendered over MCP with the
  # real Orrery (dev and GCC debug presets only: a full synth render).
  if(TARGET orrery-headless AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT SUPPLE_SANITIZE)
    add_test(NAME engine.mcp-arrangement
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/mcp_arrangement.py
              $<TARGET_FILE:${PRODUCT_SLUG}-engine> $<TARGET_FILE:orrery-headless>)
    set_tests_properties(engine.mcp-arrangement PROPERTIES LABELS "engine;slow")
  endif()
endif()
