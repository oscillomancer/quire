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

if(UNIX AND NOT APPLE)
  set(orrery_headless_exports "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/exports-clap.map")
  target_link_options(orrery-headless PRIVATE "LINKER:--version-script=${orrery_headless_exports}")
  set_property(TARGET orrery-headless APPEND PROPERTY LINK_DEPENDS "${orrery_headless_exports}")
endif()

if(SUPPLE_BUILD_TESTS)
  supple_add_tests(orrery-clap SOURCES tests/test_clap.cpp tests/test_bridge.cpp tests/test_edits.cpp DEPS supple::clap-host LABELS rt)
  add_dependencies(orrery-clap.tests orrery-headless)
  target_compile_definitions(orrery-clap.tests PRIVATE
    ORRERY_CLAP_PATH="$<TARGET_FILE:orrery-headless>")
  add_test(NAME orrery-clap.conformance
           COMMAND semantic-conformance $<TARGET_FILE:orrery-headless> com.suppleaudio.orrery)
  if(UNIX AND NOT APPLE)
    add_test(NAME orrery-clap.exports COMMAND ${CMAKE_COMMAND}
      -DMODULE=$<TARGET_FILE:orrery-headless> "-DEXPECTED=clap_entry"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/../test/check_plugin_exports.cmake")
    add_test(NAME plugin.orrery-headless.unload COMMAND clap-unload-probe
      $<TARGET_FILE:orrery-headless>)
    set_tests_properties(plugin.orrery-headless.unload PROPERTIES
      LABELS "rt;plugin-unload" TIMEOUT 30)
  endif()
  # The module the sandbox and the engine load: same reset contract as the
  # plugin (see stock-effects.md §0).
  add_test(NAME plugin.orrery-headless.reset COMMAND clap-reset-check
    $<TARGET_FILE:orrery-headless>)
  set_tests_properties(plugin.orrery-headless.reset PROPERTIES
    LABELS "rt;plugin-reset" TIMEOUT 180)
endif()
