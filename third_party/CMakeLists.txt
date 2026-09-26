# Vendored third-party code. Every directory here must have an entry in
# third_party/manifest.json and THIRD_PARTY.md (tools/license-check enforces
# this). Warnings are never enabled for these targets.

add_library(tp-clap INTERFACE)
target_include_directories(tp-clap SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/clap/include)
add_library(supple::clap ALIAS tp-clap)

add_library(tp-doctest INTERFACE)
target_include_directories(tp-doctest SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/doctest/include)
add_library(supple::doctest ALIAS tp-doctest)

add_library(tp-doctest-main STATIC ${CMAKE_CURRENT_SOURCE_DIR}/doctest_main.cpp)
target_link_libraries(tp-doctest-main PUBLIC tp-doctest)
target_compile_features(tp-doctest-main PUBLIC cxx_std_20)
if(_supple_sanitize_flags)
  target_compile_options(tp-doctest-main PRIVATE ${_supple_sanitize_flags})
endif()
add_library(supple::doctest-main ALIAS tp-doctest-main)

add_library(tp-dr-libs INTERFACE)
target_include_directories(tp-dr-libs SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/dr_libs/include)
add_library(supple::dr-libs ALIAS tp-dr-libs)

add_library(tp-pocketfft INTERFACE)
target_include_directories(tp-pocketfft SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/pocketfft/include)
target_compile_definitions(tp-pocketfft INTERFACE POCKETFFT_NO_MULTITHREADING)
add_library(supple::pocketfft ALIAS tp-pocketfft)
