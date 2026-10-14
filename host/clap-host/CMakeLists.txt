supple_add_library(clap-host
  SOURCES src/clap_host.cpp src/vst3_host.cpp src/bridge_client.cpp src/bridge_protocol.cpp src/scan.cpp
  DEPS supple::core-base supple::core-json supple::clap supple::semantic-params-abi supple::vst3-host
  PRIVATE_DEPS ${CMAKE_DL_LIBS})
# The script player's mark, which the scan cache leaves out (header only).
target_include_directories(clap-host PRIVATE ${PROJECT_SOURCE_DIR}/plugins/suite/compiler/include)

# The plugin bridge: hosts dedicated or shared isolated plugin instances (ADR-0013).
add_executable(${PRODUCT_SLUG}-bridge bridge/bridge_main.cpp)
target_link_libraries(${PRODUCT_SLUG}-bridge PRIVATE supple::clap-host supple::core-dsp)
supple_target_defaults(${PRODUCT_SLUG}-bridge)

if(SUPPLE_BUILD_TESTS)
  # Hermetic CLAP fixtures for the host tests.
  add_library(clap-host-fixtures MODULE tests/fixture_plugins.cpp)
  target_link_libraries(clap-host-fixtures PRIVATE supple::core-params supple::core-patch
                        supple::semantic-params-abi)
  set_target_properties(clap-host-fixtures PROPERTIES PREFIX "" SUFFIX ".clap" OUTPUT_NAME "test-fixtures")
  supple_target_defaults(clap-host-fixtures)
  target_compile_options(clap-host-fixtures PRIVATE -Wno-old-style-cast)

  supple_add_tests(clap-host SOURCES tests/test_clap_host.cpp tests/test_scan.cpp tests/test_main_thread_wake.cpp LABELS rt)
  add_dependencies(clap-host.tests clap-host-fixtures ${PRODUCT_SLUG}-bridge)
  target_include_directories(clap-host.tests PRIVATE ${PROJECT_SOURCE_DIR}/plugins/suite/compiler/include)
  target_compile_definitions(clap-host.tests PRIVATE FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>"
                             BRIDGE_PATH="$<TARGET_FILE:${PRODUCT_SLUG}-bridge>")

  foreach(_mode good crash hang)
    add_library(clap-scan-${_mode} MODULE tests/fixture_scan.cpp)
    target_link_libraries(clap-scan-${_mode} PRIVATE supple::clap)
    set_target_properties(clap-scan-${_mode} PROPERTIES PREFIX "" SUFFIX ".clap")
    supple_target_defaults(clap-scan-${_mode})
    add_dependencies(clap-host.tests clap-scan-${_mode})
  endforeach()
  target_compile_definitions(clap-scan-good PRIVATE SCAN_MODE=0)
  target_compile_definitions(clap-scan-crash PRIVATE SCAN_MODE=1)
  target_compile_definitions(clap-scan-hang PRIVATE SCAN_MODE=2)
  target_compile_definitions(clap-host.tests PRIVATE
    SCAN_GOOD_PATH="$<TARGET_FILE:clap-scan-good>"
    SCAN_CRASH_PATH="$<TARGET_FILE:clap-scan-crash>"
    SCAN_HANG_PATH="$<TARGET_FILE:clap-scan-hang>")

  set(_vst3_fixture_sdk ${CMAKE_SOURCE_DIR}/third_party/vst3sdk-v3.8.0_build_66/public.sdk/source/vst)
  file(GLOB _vst3_fixture_base
    ${CMAKE_SOURCE_DIR}/third_party/vst3sdk-v3.8.0_build_66/base/source/*.cpp)
  file(GLOB _vst3_fixture_threads
    ${CMAKE_SOURCE_DIR}/third_party/vst3sdk-v3.8.0_build_66/base/thread/source/*.cpp)
  add_library(clap-host-vst3-fixtures MODULE
    tests/fixture_vst3.cpp
    ${_vst3_fixture_base}
    ${_vst3_fixture_threads}
    ${CMAKE_SOURCE_DIR}/third_party/vst3sdk-v3.8.0_build_66/public.sdk/source/common/commoniids.cpp
    ${CMAKE_SOURCE_DIR}/third_party/vst3sdk-v3.8.0_build_66/public.sdk/source/common/pluginview.cpp
    ${_vst3_fixture_sdk}/vstsinglecomponenteffect.cpp
    ${_vst3_fixture_sdk}/vstbus.cpp
    ${_vst3_fixture_sdk}/vstcomponentbase.cpp
    ${_vst3_fixture_sdk}/vstparameters.cpp)
  target_link_libraries(clap-host-vst3-fixtures PRIVATE supple::vst3-host)
  set_target_properties(clap-host-vst3-fixtures PROPERTIES PREFIX "" SUFFIX ".so"
    OUTPUT_NAME "test-vst3"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-vst3.vst3/Contents/x86_64-linux")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-vst3.vst3/Contents/Resources")
  configure_file(tests/fixture_moduleinfo.json
    "${CMAKE_CURRENT_BINARY_DIR}/test-vst3.vst3/Contents/Resources/moduleinfo.json" COPYONLY)
  target_compile_options(clap-host-vst3-fixtures PRIVATE -w)
  target_compile_definitions(clap-host-vst3-fixtures PRIVATE DEVELOPMENT=1)
  add_dependencies(clap-host.tests clap-host-vst3-fixtures)
  target_compile_definitions(clap-host.tests PRIVATE
    FIXTURE_VST3_PATH="${CMAKE_CURRENT_BINARY_DIR}/test-vst3.vst3"
    FIXTURE_VST3_MODULE="$<TARGET_FILE:clap-host-vst3-fixtures>")
endif()
