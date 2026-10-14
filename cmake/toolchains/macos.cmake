# Xcode 16.4 libc++ lacks floating-point from_chars. Use the matched Homebrew
# LLVM headers and runtime for macOS 14.4 instead (ADR-0034's fallback).
# SUPPLE_MAC_LLVM_ROOT supports a user-installed LLVM in another prefix.
if(NOT SUPPLE_MAC_LLVM_ROOT)
  find_program(_supple_brew brew REQUIRED)
  execute_process(COMMAND "${_supple_brew}" --prefix llvm
    OUTPUT_VARIABLE SUPPLE_MAC_LLVM_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
endif()
if(NOT EXISTS "${SUPPLE_MAC_LLVM_ROOT}/bin/clang++")
  message(FATAL_ERROR "Install Homebrew llvm or set SUPPLE_MAC_LLVM_ROOT to a complete LLVM prefix")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SUPPLE_MAC_LLVM_ROOT)
set(CMAKE_C_COMPILER "${SUPPLE_MAC_LLVM_ROOT}/bin/clang" CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${SUPPLE_MAC_LLVM_ROOT}/bin/clang++" CACHE FILEPATH "C++ compiler" FORCE)
# Availability is disabled only with the matching bundled library. Linking to
# the SDK's libc++ instead would leave symbols missing on the minimum OS.
set(CMAKE_CXX_FLAGS_INIT "-nostdinc++ -isystem ${SUPPLE_MAC_LLVM_ROOT}/include/c++/v1 -D_LIBCPP_DISABLE_AVAILABILITY")
set(_supple_runtime_flags "-L${SUPPLE_MAC_LLVM_ROOT}/lib/c++ -L${SUPPLE_MAC_LLVM_ROOT}/lib/unwind -lunwind -Wl,-rpath,${SUPPLE_MAC_LLVM_ROOT}/lib/c++ -Wl,-rpath,${SUPPLE_MAC_LLVM_ROOT}/lib/unwind")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_supple_runtime_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_supple_runtime_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_supple_runtime_flags}")
