# The updater replaces a product's framework/ with the template's and touches
# nothing else; when it refuses, it changes nothing at all.
cmake_minimum_required(VERSION 3.27)
file(REMOVE_RECURSE "${WORK}")
set(template "${WORK}/template")
set(product "${WORK}/product")
file(MAKE_DIRECTORY "${template}/framework/cmake" "${product}/framework/tools" "${product}/product")
file(WRITE "${template}/framework/VERSION.txt" "2.0.0\n")
file(WRITE "${template}/framework/cmake/Framework.cmake" "# updated framework\n")
file(WRITE "${product}/framework/VERSION.txt" "1.0.0\n")
file(WRITE "${product}/framework/removed.txt" "only in the old framework\n")
file(COPY "${FRAMEWORK}/tools/update_framework.cmake" DESTINATION "${product}/framework/tools")
# Product-owned files, including a preset the updater must not rewrite.
file(WRITE "${product}/product/product.cmake" "# product identity stays unchanged\n")
file(WRITE "${product}/product/Processor.h" "// the product's DSP\n")
file(WRITE "${product}/CMakePresets.json" [=[
{
  "version": 3,
  "testPresets": [{"name": "asan", "environment": {"ASAN_OPTIONS": "detect_leaks=1"}}]
}
]=])

# Every file under the product, with its contents, as one string.
function(snapshot dir out)
    file(GLOB_RECURSE files RELATIVE "${dir}" "${dir}/*")
    list(SORT files)
    set(all "")
    foreach(f IN LISTS files)
        file(READ "${dir}/${f}" body)
        string(APPEND all "== ${f}\n${body}")
    endforeach()
    set(${out} "${all}" PARENT_SCOPE)
endfunction()

function(run_update from expect_success)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DFROM=${from}"
        -P "${product}/framework/tools/update_framework.cmake"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if (expect_success AND NOT status EQUAL 0)
        message(FATAL_ERROR "update failed: ${output}\n${error}")
    elseif (NOT expect_success AND status EQUAL 0)
        message(FATAL_ERROR "update from ${from} should have been refused")
    endif()
endfunction()

# Refusals: each leaves the product exactly as it was.
snapshot("${product}" before)
file(MAKE_DIRECTORY "${WORK}/not-a-template/framework")
run_update("${WORK}/not-a-template" FALSE)
run_update("${product}" FALSE)  # its own framework
file(RENAME "${product}/product/product.cmake" "${product}/product/moved.cmake")
run_update("${template}" FALSE)  # not a product
file(RENAME "${product}/product/moved.cmake" "${product}/product/product.cmake")
snapshot("${product}" after)
if (NOT after STREQUAL before)
    message(FATAL_ERROR "a refused update changed the product")
endif()

# The source must not live in a directory the updater clears for staging.
foreach(scratch .framework-update .framework-previous)
    file(COPY "${template}/" DESTINATION "${product}/${scratch}")
    snapshot("${product}" before)
    run_update("${product}/${scratch}" FALSE)
    snapshot("${product}" after)
    if (NOT after STREQUAL before)
        message(FATAL_ERROR "an update from ${scratch} destroyed its source")
    endif()
    file(REMOVE_RECURSE "${product}/${scratch}")
endforeach()

# The update: framework/ becomes the template's, and nothing else changes.
snapshot("${product}/product" product_before)
file(READ "${product}/CMakePresets.json" presets_before)
run_update("${template}" TRUE)
snapshot("${product}/framework" framework_after)
snapshot("${template}/framework" framework_expected)
if (NOT framework_after STREQUAL framework_expected)
    message(FATAL_ERROR "framework/ is not the template's:\n${framework_after}")
endif()
snapshot("${product}/product" product_after)
file(READ "${product}/CMakePresets.json" presets_after)
if (NOT product_after STREQUAL product_before OR NOT presets_after STREQUAL presets_before)
    message(FATAL_ERROR "the update changed a product-owned file")
endif()
if (EXISTS "${product}/.framework-update" OR EXISTS "${product}/.framework-previous")
    message(FATAL_ERROR "the update left its staging directories behind")
endif()
message(STATUS "Framework update replaced framework/ and only framework/")
