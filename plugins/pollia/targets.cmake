# The suite (Quire's plugins/suite: suite.cmake), and the script compiled as
# the script player's compiler runs it: integers wrap, and no fused
# multiply-adds. An Array may start from single braces (= {1.0, 0.5}), as
# the language allows, without a warning.

get_filename_component(SUPPLE_SUITE "${SUPPLE_PRODUCT}/../suite" ABSOLUTE)
include(${SUPPLE_SUITE}/suite.cmake)
if (NOT MSVC)
    set_source_files_properties(${SUPPLE_PRODUCT}/Catalog.cpp PROPERTIES
        COMPILE_OPTIONS "-ffp-contract=off;-fwrapv;-Wno-missing-braces")
endif()

if (SUPPLE_BUILD_TESTS)
    add_executable(pollia-motion-test ${SUPPLE_PRODUCT}/test/motion.cpp)
    target_link_libraries(pollia-motion-test PRIVATE suite-runtime)
    target_compile_features(pollia-motion-test PRIVATE cxx_std_20)
    supple_defaults(pollia-motion-test)
    if (NOT MSVC)
        target_compile_options(pollia-motion-test PRIVATE -ffp-contract=off -fwrapv -Wno-missing-braces)
    endif()
    add_test(NAME pollia-motion COMMAND pollia-motion-test)
endif()
