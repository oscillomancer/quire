if (MSVC AND NOT SUPPLE_SANITIZE STREQUAL "")
    message(FATAL_ERROR "SUPPLE_SANITIZE=${SUPPLE_SANITIZE} is not supported with MSVC; use a supported Clang/GCC toolchain or clear SUPPLE_SANITIZE")
endif()
