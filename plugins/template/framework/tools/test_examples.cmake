# Builds every example in the template's examples/ as a product of its own
# and runs a preset's tests in it. The examples are where the features the
# starter does not use get exercised: an instrument's notes, smoothing, output
# parameters. A framework change is not done until this passes. The examples
# live beside framework/, not in it, so products do not carry them; run this
# from the template:
#
#   cmake [-DPRESET=release] [-DEXAMPLES="pulse;instrument"] [-DCLAP_VALIDATOR=/path/to/clap-validator] \
#         -P framework/tools/test_examples.cmake
#
# clap-validator runs in each example when it is on the PATH, given, or
# known to the template's own build of the same preset.
# Each example is generated with new_product.cmake, under its own name and
# codes, into build/examples/<preset>/<example>. Later runs regenerate the
# sources there and keep its build directory, so they build incrementally.

cmake_minimum_required(VERSION 3.27)

get_filename_component(template "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if (NOT PRESET)
    set(PRESET release)
endif()
set(work "${template}/build/examples/${PRESET}")

if (NOT CLAP_VALIDATOR)
    find_program(CLAP_VALIDATOR clap-validator)
endif()
if (NOT CLAP_VALIDATOR AND EXISTS "${template}/build/${PRESET}/CMakeCache.txt")
    file(STRINGS "${template}/build/${PRESET}/CMakeCache.txt" cached REGEX "^SUPPLE_CLAP_VALIDATOR:FILEPATH=")
    string(REGEX REPLACE "^[^=]*=" "" cached "${cached}")
    if (cached AND EXISTS "${cached}")
        set(CLAP_VALIDATOR "${cached}")
    endif()
endif()
set(validator_arg "")
if (CLAP_VALIDATOR)
    set(validator_arg "-DSUPPLE_CLAP_VALIDATOR=${CLAP_VALIDATOR}")
endif()

if (NOT EXAMPLES)
    file(GLOB dirs LIST_DIRECTORIES true "${template}/examples/*")
    foreach(dir ${dirs})
        if (EXISTS "${dir}/product.cmake")
            get_filename_component(name "${dir}" NAME)
            list(APPEND EXAMPLES ${name})
        endif()
    endforeach()
endif()
if (NOT EXAMPLES)
    message(FATAL_ERROR "test_examples: no examples in ${template}/examples; run this from the template")
endif()

function(product_value file var out)
    file(STRINGS "${file}" lines REGEX "^set\\(${var} ")
    string(REGEX MATCH "\"([^\"]*)\"" _ "${lines}")
    set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# Runs one step, its output in a log; on failure, shows the log's end.
function(step result name log)
    execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${dest}" RESULT_VARIABLE code
        OUTPUT_FILE "${log}" ERROR_FILE "${log}.err")
    file(READ "${log}" out)
    file(READ "${log}.err" err)
    file(WRITE "${log}" "${out}${err}")
    file(REMOVE "${log}.err")
    if (NOT code EQUAL 0)
        string(LENGTH "${out}${err}" length)
        set(tail "${out}${err}")
        if (length GREATER 6000)
            math(EXPR from "${length} - 6000")
            string(SUBSTRING "${out}${err}" ${from} -1 tail)
        endif()
        message("${tail}")
        message("  ${name} FAILED (full log: ${log})")
        set(${result} FALSE PARENT_SCOPE)
    else()
        set(${result} TRUE PARENT_SCOPE)
    endif()
endfunction()

set(failed "")
foreach(example ${EXAMPLES})
    set(source "${template}/examples/${example}")
    if (NOT EXISTS "${source}/product.cmake")
        message(FATAL_ERROR "test_examples: no example called ${example}")
    endif()
    product_value("${source}/product.cmake" PRODUCT_NAME name)
    product_value("${source}/product.cmake" PRODUCT_ID id)
    product_value("${source}/product.cmake" PRODUCT_AU_SUBTYPE code)
    set(dest "${work}/${example}")
    set(fresh "${work}/${example}.new")

    # Regenerate the sources; keep the build directory.
    file(REMOVE_RECURSE "${fresh}")
    execute_process(COMMAND ${CMAKE_COMMAND} -DNAME=${name} -DID=${id} -DCODE=${code} -DDEST=${fresh}
            -DEXAMPLE=${example} -DLICENSE=MIT -DALLOW_EXAMPLE_CODE=ON
            -P "${template}/framework/tools/new_product.cmake"
        RESULT_VARIABLE code_generated OUTPUT_QUIET)
    if (NOT code_generated EQUAL 0)
        message(FATAL_ERROR "test_examples: could not generate ${example}")
    endif()
    if (EXISTS "${dest}")
        if (NOT EXISTS "${dest}/.supple-example")
            message(FATAL_ERROR "test_examples: ${dest} exists and was not made by this tool; move it away")
        endif()
        file(GLOB entries LIST_DIRECTORIES true "${dest}/*" "${dest}/.*")
        foreach(entry ${entries})
            get_filename_component(entry_name "${entry}" NAME)
            if (NOT entry_name STREQUAL "build")
                file(REMOVE_RECURSE "${entry}")
            endif()
        endforeach()
    endif()
    file(MAKE_DIRECTORY "${dest}")
    file(COPY "${fresh}/" DESTINATION "${dest}")
    file(REMOVE_RECURSE "${fresh}")
    # new_product leaves the benchmark baseline behind; an example keeps its own.
    if (EXISTS "${source}/benchmark.txt")
        file(COPY_FILE "${source}/benchmark.txt" "${dest}/product/benchmark.txt")
    endif()
    file(WRITE "${dest}/.supple-example" "Generated by framework/tools/test_examples.cmake; replaced on every run.\n")

    message(STATUS "${example}: configuring, building and testing with the ${PRESET} preset")
    step(ok "${example} configure" "${work}/${example}-configure.log" ${CMAKE_COMMAND} --preset ${PRESET} ${validator_arg})
    if (ok)
        step(ok "${example} build" "${work}/${example}-build.log" ${CMAKE_COMMAND} --build --preset ${PRESET})
    endif()
    if (ok)
        step(ok "${example} tests" "${work}/${example}-test.log" ${CMAKE_CTEST_COMMAND} --preset ${PRESET} --output-on-failure)
    endif()
    if (ok)
        file(STRINGS "${work}/${example}-test.log" summary REGEX "tests passed")
        message(STATUS "${example}: ${summary}")
    else()
        list(APPEND failed ${example})
    endif()
endforeach()

if (failed)
    list(JOIN failed ", " names)
    message(FATAL_ERROR "test_examples (${PRESET}): ${names} failed")
endif()
message(STATUS "test_examples (${PRESET}): every example passed")
