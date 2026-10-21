# The suite in a plugin of the template, included from a product's
# targets.cmake with SUPPLE_SUITE set to this folder: the script runtime,
# the semantic-params documents and layer, and the glue that runs scripts,
# added to the product's targets. The player adds the script compiler; a
# source export adds its script.

# Exports carry the same MIT core-base parser and fast_float headers. The
# player uses those files directly from the monorepo, without a second parser.
set(suite_chars ${SUPPLE_SUITE}/base)
set(suite_fast_float ${SUPPLE_SUITE}/third_party/fast_float)
if (NOT EXISTS "${suite_chars}/include/supple/base/chars.h")
    set(suite_chars ${SUPPLE_SUITE}/../../libs/core-base)
    set(suite_fast_float ${SUPPLE_SUITE}/../../third_party/fast_float)
endif()

add_library(suite-runtime STATIC ${SUPPLE_SUITE}/runtime/src/runtime.cpp)
target_include_directories(suite-runtime PUBLIC ${SUPPLE_SUITE}/runtime/include)
target_include_directories(suite-runtime SYSTEM PRIVATE ${SUPPLE_FRAMEWORK})
# Arithmetic as the script compiler's code does it: no fused multiply-adds.
if (NOT MSVC)
    target_compile_options(suite-runtime PRIVATE -ffp-contract=off)
endif()
supple_defaults(suite-runtime)

target_sources(product-core PRIVATE
    ${suite_chars}/src/chars.cpp
    ${SUPPLE_SUITE}/semantic/src/json.cpp
    ${SUPPLE_SUITE}/semantic/src/semantics.cpp
    ${SUPPLE_SUITE}/glue/src/script_definition.cpp)
target_include_directories(product-core PUBLIC ${SUPPLE_SUITE}/semantic/include ${SUPPLE_SUITE}/glue/include)
target_include_directories(product-core PRIVATE ${suite_chars}/include)
target_include_directories(product-core SYSTEM PRIVATE ${suite_fast_float}/include)
target_link_libraries(product-core PUBLIC suite-runtime)
if (TARGET product-impl)
    target_sources(product-impl PRIVATE
        ${SUPPLE_SUITE}/semantic/src/semantic_layer.cpp
        ${SUPPLE_SUITE}/glue/src/script_layer.cpp)
    # A script's own editor, and the route list, where there is a GUI.
    if (TARGET product-gui)
        target_sources(product-impl PRIVATE ${SUPPLE_SUITE}/glue/src/script_editor.cpp
                                            ${SUPPLE_SUITE}/glue/src/route_list.cpp)
    endif()
endif()
