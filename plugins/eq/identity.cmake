set(EQ_DISPLAY_NAME "Facet")
configure_file("${CMAKE_CURRENT_LIST_DIR}/engine/include/eq/identity.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/eq/identity.h" @ONLY)
