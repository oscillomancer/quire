cmake_minimum_required(VERSION 3.25)

include(cmake/Product.cmake)
project(${PRODUCT_SLUG} VERSION ${PRODUCT_VERSION} LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SUPPLE_BUILD_FUZZERS "Build parser fuzz targets" OFF)
option(SUPPLE_BUILD_TESTS "Build unit tests" ON)
option(SUPPLE_BUILD_PLUGINS "Build the stock plugins (template-derived)" ON)
option(SUPPLE_BUILD_GUI "Build GUI components" ON)
option(SUPPLE_BUILD_SPIKES "Build experimental research spikes" OFF)

include(cmake/Defaults.cmake)

if(SUPPLE_BUILD_TESTS)
  enable_testing()
endif()

# Generated identity header: <supple/product.h>
configure_file(cmake/product.h.in ${CMAKE_BINARY_DIR}/generated/supple/product.h @ONLY)
add_library(product-info INTERFACE)
target_include_directories(product-info INTERFACE ${CMAKE_BINARY_DIR}/generated)
add_library(supple::product-info ALIAS product-info)

add_subdirectory(third_party)
add_subdirectory(libs)
add_subdirectory(host)
add_subdirectory(tests)
if(SUPPLE_BUILD_SPIKES)
  add_subdirectory(tools/spikes/gui-shell)
endif()
if(SUPPLE_BUILD_FUZZERS)
  add_subdirectory(tests/fuzz)
endif()

# License gate: fails the build when a dependency is not declared.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_custom_target(license-check ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/license-check/check.py --root ${CMAKE_SOURCE_DIR}
    COMMENT "Checking third-party licenses"
    VERBATIM)
  if(SUPPLE_BUILD_TESTS)
    add_test(NAME license-check
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/license-check/check.py --root ${CMAKE_SOURCE_DIR})
  endif()
endif()
