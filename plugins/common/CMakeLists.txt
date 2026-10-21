# The semantic-params extension ABI (C header), shared by plugins and hosts.
add_library(semantic-params-abi INTERFACE)
target_include_directories(semantic-params-abi INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(semantic-params-abi INTERFACE supple::clap)
add_library(supple::semantic-params-abi ALIAS semantic-params-abi)

if(SUPPLE_BUILD_TESTS)
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
  endif()
endif()
