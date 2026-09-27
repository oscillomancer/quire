supple_add_library(clap-host
  SOURCES src/clap_host.cpp src/bridge_client.cpp src/bridge_protocol.cpp
  DEPS supple::core-base supple::core-json supple::clap supple::semantic-params-abi
  PRIVATE_DEPS ${CMAKE_DL_LIBS})

# The plugin bridge: hosts one isolated plugin instance (ADR-0013).
add_executable(${PRODUCT_SLUG}-bridge bridge/bridge_main.cpp)
target_link_libraries(${PRODUCT_SLUG}-bridge PRIVATE supple::clap-host)
supple_target_defaults(${PRODUCT_SLUG}-bridge)

if(SUPPLE_BUILD_TESTS)
  # Hermetic CLAP fixtures for the host tests.
  add_library(clap-host-fixtures MODULE tests/fixture_plugins.cpp)
  target_link_libraries(clap-host-fixtures PRIVATE supple::core-params supple::core-patch
                        supple::semantic-params-abi)
  set_target_properties(clap-host-fixtures PROPERTIES PREFIX "" SUFFIX ".clap" OUTPUT_NAME "test-fixtures")
  supple_target_defaults(clap-host-fixtures)
  target_compile_options(clap-host-fixtures PRIVATE -Wno-old-style-cast)

  supple_add_tests(clap-host SOURCES tests/test_clap_host.cpp LABELS rt)
  add_dependencies(clap-host.tests clap-host-fixtures ${PRODUCT_SLUG}-bridge)
  target_compile_definitions(clap-host.tests PRIVATE FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>"
                             BRIDGE_PATH="$<TARGET_FILE:${PRODUCT_SLUG}-bridge>")
endif()
