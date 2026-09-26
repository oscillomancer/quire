add_subdirectory(model)

supple_add_library(engine
  SOURCES src/offline.cpp src/plan.cpp src/realtime.cpp
  DEPS supple::model supple::clap-host supple::audio-file supple::timeline supple::semantic-params-abi)

if(SUPPLE_BUILD_TESTS)
  supple_add_tests(engine SOURCES tests/test_offline.cpp tests/test_realtime.cpp LABELS rt)
  add_dependencies(engine.tests clap-host-fixtures)
  target_compile_definitions(engine.tests PRIVATE FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>")
endif()
