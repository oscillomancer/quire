# The suite in a player build: the script runtime and compiler (with MIR),
# the semantic documents and layer, and the glue that runs scripts. The
# suite is the folder above this one (Quire's plugins/suite); SUPPLE_MIR_DIR
# is where MIR's sources are (Quire's third_party/mir-1.0.0).

get_filename_component(SUPPLE_SUITE "${SUPPLE_PRODUCT}/.." ABSOLUTE)
set(SUPPLE_MIR_DIR "${SUPPLE_SUITE}/../../third_party/mir-1.0.0" CACHE PATH "MIR 1.0.0's sources")

add_library(suite-mir STATIC ${SUPPLE_MIR_DIR}/mir.c ${SUPPLE_MIR_DIR}/mir-gen.c)
target_include_directories(suite-mir SYSTEM PUBLIC ${SUPPLE_MIR_DIR})
set_target_properties(suite-mir PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(suite-mir PRIVATE -w -fsigned-char)
supple_vendored(suite-mir)
supple_sanitize(suite-mir)
if (SUPPLE_SANITIZE MATCHES "address")
    # MIR's string hash reads unaligned words, which x86-64 and AArch64 allow.
    target_compile_options(suite-mir PRIVATE -fno-sanitize=alignment)
endif()

add_library(suite-script STATIC
    ${SUPPLE_SUITE}/runtime/src/runtime.cpp
    ${SUPPLE_SUITE}/compiler/src/lexer.cpp
    ${SUPPLE_SUITE}/compiler/src/parser.cpp
    ${SUPPLE_SUITE}/compiler/src/builtins.cpp
    ${SUPPLE_SUITE}/compiler/src/sema.cpp
    ${SUPPLE_SUITE}/compiler/src/sema_expr.cpp
    ${SUPPLE_SUITE}/compiler/src/codegen.cpp
    ${SUPPLE_SUITE}/compiler/src/compiler.cpp)
target_include_directories(suite-script PUBLIC ${SUPPLE_SUITE}/runtime/include ${SUPPLE_SUITE}/compiler/include
                                        PRIVATE ${SUPPLE_SUITE}/compiler/src)
target_include_directories(suite-script SYSTEM PRIVATE ${SUPPLE_FRAMEWORK})
target_link_libraries(suite-script PRIVATE suite-mir)
supple_defaults(suite-script)

target_sources(product-core PRIVATE ${SUPPLE_SUITE}/semantic/src/json.cpp ${SUPPLE_SUITE}/semantic/src/semantics.cpp
                                    ${SUPPLE_SUITE}/glue/src/script_definition.cpp)
target_include_directories(product-core PUBLIC ${SUPPLE_SUITE}/semantic/include ${SUPPLE_SUITE}/glue/include)
# dladdr (product/Catalog.cpp), in libdl before glibc 2.34.
target_link_libraries(product-core PUBLIC suite-script ${CMAKE_DL_LIBS})
if (TARGET product-impl)
    target_sources(product-impl PRIVATE ${SUPPLE_SUITE}/semantic/src/semantic_layer.cpp
                                        ${SUPPLE_SUITE}/glue/src/script_layer.cpp)
endif()

if (SUPPLE_BUILD_TESTS AND COMMAND supple_test)
    # Scripts the tests play (SUPPLE_SCRIPT_LIBRARY): host-smoke and the
    # VST3 checks the one with the player's own ID, the rest every one.
    set(player_smoke ${SUPPLE_PRODUCT}/test/smoke)
    set(player_library "${player_smoke}:${SUPPLE_PRODUCT}/test/library")
    if (TARGET processor-tests)
        target_include_directories(processor-tests PRIVATE ${SUPPLE_SUITE}/compiler/tests)
        set_tests_properties(processor-tests PROPERTIES ENVIRONMENT "SUPPLE_SCRIPT_LIBRARY=${player_library}")
    endif()
    foreach(test host-smoke-2ch host-smoke-1ch vst3-unload exports-clap exports-vst3 x11-embedding)
        if (TEST ${test})
            set_tests_properties(${test} PROPERTIES ENVIRONMENT "SUPPLE_SCRIPT_LIBRARY=${player_smoke}")
        endif()
    endforeach()
    if (SUPPLE_BUILD_PLUGIN)
        supple_test(player-plugin-tests SOURCES ${SUPPLE_PRODUCT}/test/plugin_test.cpp LINK product-impl)
        set_tests_properties(player-plugin-tests PROPERTIES ENVIRONMENT "SUPPLE_SCRIPT_LIBRARY=${player_library}")
    endif()
endif()
