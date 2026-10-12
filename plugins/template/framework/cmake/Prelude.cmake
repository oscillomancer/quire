# Runs before project(): validates the product identity, derives what follows
# from it, and sets the variables that must exist before languages are
# enabled.

foreach(var PRODUCT_NAME PRODUCT_ID PRODUCT_VERSION PRODUCT_DESCRIPTION PRODUCT_VENDOR PRODUCT_KIND
        PRODUCT_AU_MANUFACTURER PRODUCT_AU_SUBTYPE PRODUCT_STATE_TAG)
    if (NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "product/product.cmake must set ${var}")
    endif()
endforeach()

if (NOT PRODUCT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "PRODUCT_VERSION must be MAJOR.MINOR.PATCH, not '${PRODUCT_VERSION}'")
endif()
if (NOT PRODUCT_KIND MATCHES "^(effect|instrument|note-effect)$")
    message(FATAL_ERROR "PRODUCT_KIND must be 'effect', 'instrument' or 'note-effect', not '${PRODUCT_KIND}'")
endif()
if (NOT PRODUCT_KIND STREQUAL "note-effect" AND "${PRODUCT_CHANNELS}" STREQUAL "")
    message(FATAL_ERROR "product/product.cmake must set PRODUCT_CHANNELS")
endif()
if (NOT PRODUCT_ID MATCHES "^[A-Za-z0-9-]+(\\.[A-Za-z0-9-]+)+$")
    message(FATAL_ERROR "PRODUCT_ID must be reverse-DNS, e.g. com.suppleaudio.product")
endif()
foreach(code PRODUCT_AU_MANUFACTURER PRODUCT_AU_SUBTYPE PRODUCT_STATE_TAG)
    string(LENGTH "${${code}}" length)
    if (NOT length EQUAL 4 OR NOT "${${code}}" MATCHES "^[!-~]+$")
        message(FATAL_ERROR "${code} must be exactly four printable ASCII characters, not '${${code}}'")
    endif()
endforeach()
# PRODUCT_CHANNELS and the capability switches are checked by
# cmake/Capabilities.cmake.

# The lowercase identifier form of the name: CMake project, Objective-C class
# prefix, install paths. Derived unless the product sets it.
if (NOT PRODUCT_CODE)
    string(TOLOWER "${PRODUCT_NAME}" PRODUCT_CODE)
    string(REGEX REPLACE "[^a-z0-9]" "" PRODUCT_CODE "${PRODUCT_CODE}")
endif()
if (NOT PRODUCT_CODE MATCHES "^[a-z][a-z0-9]*$")
    message(FATAL_ERROR "PRODUCT_CODE '${PRODUCT_CODE}' must start with a letter; set it explicitly")
endif()

# Before project(): project() creates the cache entry, after which a plain
# set() is silently ignored. macOS 11 is the first release that runs arm64.
if (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

# Windows: link the C++ runtime statically into every binary, so a plugin
# needs no Visual C++ redistributable on the user's machine. clap-wrapper
# builds its own libraries this way unless told otherwise, and one binary
# cannot mix the two (LNK2038, "mismatch detected for 'RuntimeLibrary'").
if (NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# A bare `cmake -B build` sets no build type on single-config generators and
# silently produces an unoptimised plugin.
get_property(_supple_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (NOT _supple_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "No CMAKE_BUILD_TYPE given; defaulting to Release")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
