# Prints the product identity CI needs, as key=value lines for $GITHUB_OUTPUT,
# read from product/product.cmake itself rather than scraped with a regex.
#   cmake -P .github/scripts/product-info.cmake
include(${CMAKE_CURRENT_LIST_DIR}/../../product/product.cmake)

# As framework/cmake/Capabilities.cmake decides it.
if (PRODUCT_KIND STREQUAL "instrument")
    set(au_type aumu)
elseif (PRODUCT_KIND STREQUAL "note-effect")
    set(au_type aumi)
elseif (PRODUCT_NOTE_INPUT)
    set(au_type aumf)
else()
    set(au_type aufx)
endif()

foreach(line
        "name=${PRODUCT_NAME}"
        "id=${PRODUCT_ID}"
        "kind=${PRODUCT_KIND}"
        "au_type=${au_type}"
        "au_subtype=${PRODUCT_AU_SUBTYPE}"
        "au_manufacturer=${PRODUCT_AU_MANUFACTURER}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E echo "${line}")
endforeach()
