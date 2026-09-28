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

# PUC Lua 5.4 (MIT) — the embedded scripting language (ADR-0006). Only the
# core and the libraries our sandbox opens are compiled: io, os, debug and
# package are not built at all, so a script cannot reach them (libs/lua-bus).
# Warnings are never enabled for third-party targets (see the file header).
set(_lua ${CMAKE_CURRENT_SOURCE_DIR}/lua-5.4.9/src)
add_library(tp-lua STATIC
  ${_lua}/lapi.c ${_lua}/lcode.c ${_lua}/lctype.c ${_lua}/ldebug.c ${_lua}/ldo.c
  ${_lua}/ldump.c ${_lua}/lfunc.c ${_lua}/lgc.c ${_lua}/llex.c ${_lua}/lmem.c
  ${_lua}/lobject.c ${_lua}/lopcodes.c ${_lua}/lparser.c ${_lua}/lstate.c
  ${_lua}/lstring.c ${_lua}/ltable.c ${_lua}/ltm.c ${_lua}/lundump.c ${_lua}/lvm.c
  ${_lua}/lzio.c
  ${_lua}/lauxlib.c ${_lua}/lbaselib.c ${_lua}/lcorolib.c ${_lua}/lmathlib.c
  ${_lua}/lstrlib.c ${_lua}/ltablib.c ${_lua}/lutf8lib.c)
target_include_directories(tp-lua SYSTEM PUBLIC ${_lua})
target_compile_features(tp-lua PUBLIC cxx_std_20)
set_target_properties(tp-lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(_supple_sanitize_flags)
  target_compile_options(tp-lua PRIVATE ${_supple_sanitize_flags})
endif()
target_link_libraries(tp-lua PUBLIC ${CMAKE_DL_LIBS})
add_library(supple::lua ALIAS tp-lua)

set(_vst3 ${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk-v3.8.0_build_66)
add_library(tp-vst3-host STATIC
  ${_vst3}/pluginterfaces/base/coreiids.cpp
  ${_vst3}/pluginterfaces/base/funknown.cpp
  ${_vst3}/pluginterfaces/base/ustring.cpp
  ${_vst3}/public.sdk/source/common/commonstringconvert.cpp
  ${_vst3}/public.sdk/source/vst/vstinitiids.cpp
  ${_vst3}/public.sdk/source/vst/hosting/module.cpp
  ${_vst3}/public.sdk/source/vst/hosting/module_linux.cpp
  ${_vst3}/public.sdk/source/vst/hosting/eventlist.cpp
  ${_vst3}/public.sdk/source/vst/hosting/hostclasses.cpp
  ${_vst3}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
  ${_vst3}/public.sdk/source/vst/hosting/parameterchanges.cpp
  ${_vst3}/public.sdk/source/vst/utility/stringconvert.cpp)
target_include_directories(tp-vst3-host SYSTEM PUBLIC ${_vst3})
target_compile_features(tp-vst3-host PUBLIC cxx_std_20)
set_target_properties(tp-vst3-host PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(_supple_sanitize_flags)
  target_compile_options(tp-vst3-host PRIVATE ${_supple_sanitize_flags})
endif()
target_link_libraries(tp-vst3-host PUBLIC ${CMAKE_DL_LIBS})
add_library(supple::vst3-host ALIAS tp-vst3-host)
