# The semantic-params extension ABI (C header), shared by plugins and hosts.
add_library(semantic-params-abi INTERFACE)
target_include_directories(semantic-params-abi INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(semantic-params-abi INTERFACE supple::clap)
add_library(supple::semantic-params-abi ALIAS semantic-params-abi)

# Plugin-side support for stock plugins (ParamStore, ClapParams, FlatSemantic).
supple_add_library(plugin-support
  SOURCES src/plugin_support.cpp
  DEPS supple::core-params supple::core-patch supple::semantic-params-abi)

if(SUPPLE_BUILD_TESTS)
  supple_add_tests(plugin-support SOURCES tests/test_plugin_support.cpp LABELS rt)

  # Checks any plugin's semantic extension through the real host (spec §7).
  add_executable(semantic-conformance tools/semantic_conformance.cpp)
  target_link_libraries(semantic-conformance PRIVATE supple::clap-host supple::core-json)
  supple_target_defaults(semantic-conformance)

  add_library(plugin-support-fixture MODULE tests/helper_fixture.cpp)
  target_link_libraries(plugin-support-fixture PRIVATE supple::plugin-support)
  set_target_properties(plugin-support-fixture PROPERTIES PREFIX "" SUFFIX ".clap" OUTPUT_NAME "tilt-fixture")
  supple_target_defaults(plugin-support-fixture)

  add_test(NAME plugin-support.conformance
           COMMAND semantic-conformance $<TARGET_FILE:plugin-support-fixture> com.suppleaudio.test.tilt)
endif()
