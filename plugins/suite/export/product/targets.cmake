# The suite (suite/suite.cmake), and the script compiled as the script
# player's compiler runs it: integers wrap, and no fused multiply-adds. An Array may start from single braces (= {1.0, 0.5}), as
# the language allows, without a warning.

set(SUPPLE_SUITE ${SUPPLE_ROOT}/suite)
include(${SUPPLE_SUITE}/suite.cmake)
if (NOT MSVC)
    set_source_files_properties(${SUPPLE_PRODUCT}/Catalog.cpp PROPERTIES
        COMPILE_OPTIONS "-ffp-contract=off;-fwrapv;-Wno-missing-braces")
endif()
