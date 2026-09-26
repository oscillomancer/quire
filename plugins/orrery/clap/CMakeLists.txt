# Orrery's CLAP plugin core (params, state, ports, semantic extension) and the
# headless module orrery-headless.clap used by tests, the sandbox and the
# engine. The template-derived plugin (with the editor) links orrery-clap.
supple_add_library(orrery-clap
  SOURCES src/orrery_clap.cpp src/edits.cpp
  DEPS supple::orrery-engine supple::plugin-support)

add_library(orrery-headless MODULE src/entry.cpp)
target_link_libraries(orrery-headless PRIVATE supple::orrery-clap)
set_target_properties(orrery-headless PROPERTIES PREFIX "" SUFFIX ".clap"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/orrery")
supple_target_defaults(orrery-headless)

if(SUPPLE_BUILD_TESTS)
  supple_add_tests(orrery-clap SOURCES tests/test_clap.cpp tests/test_bridge.cpp tests/test_edits.cpp DEPS supple::clap-host LABELS rt)
  add_dependencies(orrery-clap.tests orrery-headless)
  target_compile_definitions(orrery-clap.tests PRIVATE
    ORRERY_CLAP_PATH="$<TARGET_FILE:orrery-headless>")
  add_test(NAME orrery-clap.conformance
           COMMAND semantic-conformance $<TARGET_FILE:orrery-headless> com.suppleaudio.orrery)
endif()
