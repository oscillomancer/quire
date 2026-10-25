# The semantic-params extension ABI (C header), shared by plugins and hosts.
add_library(semantic-params-abi INTERFACE)
target_include_directories(semantic-params-abi INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(semantic-params-abi INTERFACE supple::clap)
add_library(supple::semantic-params-abi ALIAS semantic-params-abi)

if(SUPPLE_BUILD_TESTS)
  # The extension headers' two copies (host and plugins) stay the same.
  add_test(NAME semantic-params-abi.copies
    COMMAND ${CMAKE_COMMAND} -DHOST=${CMAKE_CURRENT_SOURCE_DIR}/include/supple/clap
            -DPLUGIN=${CMAKE_CURRENT_SOURCE_DIR}/../suite/semantic/include/supple/clap
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/header_copies.cmake)

  if(UNIX AND NOT APPLE)
    add_executable(clap-unload-probe tools/clap_unload_probe.cpp)
    target_link_libraries(clap-unload-probe PRIVATE supple::clap ${CMAKE_DL_LIBS})
    supple_target_defaults(clap-unload-probe)
  endif()

  if(SUPPLE_PLUGIN_HOST)
  # Checks any plugin's semantic extension through the real host (spec §7).
  add_executable(semantic-conformance tools/semantic_conformance.cpp)
  target_link_libraries(semantic-conformance PRIVATE supple::clap-host supple::core-json)
  supple_target_defaults(semantic-conformance)

  # Checks that a render after reset() depends only on the state set for it
  # (host/sandbox/src/render.cpp reuses one instance across renders).
  add_executable(clap-reset-check tools/clap_reset_check.cpp)
  target_link_libraries(clap-reset-check PRIVATE supple::clap-host)
  supple_target_defaults(clap-reset-check)

  # Checks that output does not depend on the host's block size under
  # sample-accurate automation (the engine's real-time/offline match).
  add_executable(clap-block-check tools/clap_block_check.cpp)
  target_link_libraries(clap-block-check PRIVATE supple::clap-host)
  supple_target_defaults(clap-block-check)

  # Damaged saved state, refused cleanly or accepted whole.
  add_executable(clap-state-check tools/clap_state_check.cpp)
  target_link_libraries(clap-state-check PRIVATE supple::clap-host supple::core-json)
  supple_target_defaults(clap-state-check)

  # A plugin's cost and its output per setup, to keep a change's sound
  # (plugins/AGENTS.md); run by hand, not a test.
  add_executable(clap-bench tools/clap_bench.cpp)
  target_link_libraries(clap-bench PRIVATE supple::clap-host supple::core-json)
  supple_target_defaults(clap-bench)
  endif()
endif()
