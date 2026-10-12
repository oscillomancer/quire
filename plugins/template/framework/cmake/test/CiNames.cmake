cmake_minimum_required(VERSION 3.27)
file(READ "${WORKFLOW}" workflow)
if (workflow MATCHES "steps.product.outputs.(name|au_type|au_subtype|au_manufacturer)")
    message(FATAL_ERROR "product identity is still interpolated into shell source")
endif()
# Exercise the workflow's actual transport into GITHUB_ENV with shell
# metacharacters that are legal in product filenames.
string(REGEX MATCH "sed 's/\\^/CI_PRODUCT_/'[^\n]+" transport "${workflow}")
if (transport STREQUAL "")
    message(FATAL_ERROR "workflow no longer exports product identity safely")
endif()
file(MAKE_DIRECTORY "${WORK}")
file(WRITE "${WORK}/product.env" "name=O'Brien & Tone `literal`\n")
file(REMOVE "${WORK}/github.env")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "RUNNER_TEMP=${WORK}" "GITHUB_ENV=${WORK}/github.env"
    bash -c "${transport}" COMMAND_ERROR_IS_FATAL ANY)
file(READ "${WORK}/github.env" actual)
if (NOT actual STREQUAL "CI_PRODUCT_name=O'Brien & Tone `literal`\n")
    message(FATAL_ERROR "workflow changed the product name: ${actual}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "CI_PRODUCT_name=O'Brien & Tone `literal`"
    bash -c "name=\"$CI_PRODUCT_name\"; printf '%s' \"$name\""
    OUTPUT_VARIABLE name COMMAND_ERROR_IS_FATAL ANY)
if (NOT name STREQUAL "O'Brien & Tone `literal`")
    message(FATAL_ERROR "shell interpreted product name syntax")
endif()
