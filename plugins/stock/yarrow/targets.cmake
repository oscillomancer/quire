# The suite (Quire's plugins/suite: suite.cmake), and the script compiled as
# the script player's compiler runs it: integers wrap, and no fused
# multiply-adds.

get_filename_component(SUPPLE_SUITE "${SUPPLE_PRODUCT}/../../suite" ABSOLUTE)
include(${SUPPLE_SUITE}/suite.cmake)
if (NOT MSVC)
    set_source_files_properties(${SUPPLE_PRODUCT}/Catalog.cpp PROPERTIES
        COMPILE_OPTIONS "-ffp-contract=off;-fwrapv")
endif()
