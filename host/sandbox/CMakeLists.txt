supple_add_library(sandbox
  SOURCES src/render.cpp src/session.cpp
  DEPS supple::clap-host supple::audio-file supple::core-json supple::semantic-params-abi)

if(SUPPLE_BUILD_TESTS)
  supple_add_tests(sandbox SOURCES tests/test_render.cpp tests/test_session.cpp LABELS rt)
  add_dependencies(sandbox.tests clap-host-fixtures)
  target_compile_definitions(sandbox.tests PRIVATE FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>")
endif()
