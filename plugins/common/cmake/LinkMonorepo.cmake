# Build the same first-party library targets inside a template-derived plugin's
# isolated CMake project. Call once, after project(), with SUPPLE_ROOT set.
# The plugin links supple::plugin-support; transitive dependencies supply the
# core libraries and their include directories. No network access is needed.
function(supple_plugin_add_monorepo_libraries)
    if (TARGET supple::plugin-support)
        return()
    endif()
    if (NOT SUPPLE_ROOT OR NOT EXISTS "${SUPPLE_ROOT}/cmake/Defaults.cmake")
        message(FATAL_ERROR "SUPPLE_ROOT must point at the monorepo checkout")
    endif()
    include("${SUPPLE_ROOT}/cmake/Product.cmake")
    include("${SUPPLE_ROOT}/cmake/Defaults.cmake")
    configure_file("${SUPPLE_ROOT}/cmake/product.h.in"
                   "${CMAKE_BINARY_DIR}/generated/supple/product.h" @ONLY)
    add_library(product-info INTERFACE)
    target_include_directories(product-info INTERFACE "${CMAKE_BINARY_DIR}/generated")
    add_library(supple::product-info ALIAS product-info)

    # A plugin owns its tests; the core libraries' suites are run by the parent.
    set(SUPPLE_BUILD_TESTS OFF)
    add_subdirectory("${SUPPLE_ROOT}/third_party" "${CMAKE_BINARY_DIR}/supple/third_party")
    foreach(lib core-base core-json core-schema core-params core-patch core-dsp)
        add_subdirectory("${SUPPLE_ROOT}/libs/${lib}" "${CMAKE_BINARY_DIR}/supple/${lib}")
    endforeach()
    add_subdirectory("${SUPPLE_ROOT}/plugins/common" "${CMAKE_BINARY_DIR}/supple/plugin-support")
endfunction()
