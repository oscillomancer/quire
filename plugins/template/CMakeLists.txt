cmake_minimum_required(VERSION 3.27)

# Everything product-specific lives in product/product.cmake; everything else
# in framework/cmake. This file only has to exist because project() must be
# called from the top-level CMakeLists.txt. A build may name another product
# folder, outside this tree: -DSUPPLE_PRODUCT_DIR=/path/to/product.
set(SUPPLE_PRODUCT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/product" CACHE PATH "The product's folder")
include(${SUPPLE_PRODUCT_DIR}/product.cmake)
include(framework/cmake/Prelude.cmake)

if (APPLE)
    project(${PRODUCT_CODE} VERSION ${PRODUCT_VERSION} LANGUAGES C CXX OBJC OBJCXX)
else()
    project(${PRODUCT_CODE} VERSION ${PRODUCT_VERSION} LANGUAGES C CXX)
endif()

include(framework/cmake/Framework.cmake)
