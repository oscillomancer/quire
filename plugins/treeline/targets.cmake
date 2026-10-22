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
    supple_test(script-meter-tests SOURCES ${SUPPLE_PRODUCT}/tests/meters.cpp LINK suite-runtime)
    if (NOT MSVC)
        target_compile_options(script-meter-tests PRIVATE
            -ffp-contract=off -fwrapv -Wno-missing-braces)
    endif()
endif()
