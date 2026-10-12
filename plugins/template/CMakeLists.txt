cmake_minimum_required(VERSION 3.27)

# Everything product-specific lives in product/product.cmake; everything else
# in framework/cmake. This file only has to exist because project() must be
# called from the top-level CMakeLists.txt.
include(product/product.cmake)
include(framework/cmake/Prelude.cmake)

if (APPLE)
    project(${PRODUCT_CODE} VERSION ${PRODUCT_VERSION} LANGUAGES C CXX OBJC OBJCXX)
else()
    project(${PRODUCT_CODE} VERSION ${PRODUCT_VERSION} LANGUAGES C CXX)
endif()

include(framework/cmake/Framework.cmake)
