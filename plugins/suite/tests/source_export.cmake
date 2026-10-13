# A source export end to end (ctest plugin.export.source): the sandbox
# exports SCRIPT as a C++ project, whose suite is then updated from the
# export kit KIT (suite/tools/update_suite.cmake), and which is configured,
# built and tested as a user would, offline, with this repository's
# third_party copies. Then the built plugin and the script player play the
# same signals (dsp.test, with PATCH) and must write the same bytes.
#
#   cmake -DSANDBOX=<sandbox CLI> -DPLAYER=<Script Player.clap> -DKIT=<export kit>
#         -DSCRIPT=<script.cpp> -DPATCH=<a patch's params, JSON> -DWORK=<folder>
#         -DTHIRD_PARTY=<third_party> -DCC=<C compiler> -DCXX=<C++ compiler>
#         -DBUILD_TYPE=<type> -P source_export.cmake

foreach(var SANDBOX PLAYER KIT SCRIPT PATCH WORK THIRD_PARTY CC CXX BUILD_TYPE)
    if (NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "source_export: -D${var}=... is required")
    endif()
endforeach()

function(run what)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if (NOT result EQUAL 0)
        message(FATAL_ERROR "${what} failed (${result}):\n${out}\n${err}")
    endif()
    set(output "${out}" PARENT_SCOPE)
endfunction()

get_filename_component(name "${SCRIPT}" NAME_WE)
file(REMOVE_RECURSE "${WORK}")
file(COPY "${SCRIPT}" DESTINATION "${WORK}/workdir/scripts/${name}")

run("the export" "${SANDBOX}" exec
    "{\"command\": \"dsp.export\", \"args\": {\"script\": \"${name}\", \"format\": \"source\", \"folder\": \"${WORK}/project\"}}"
    --workdir "${WORK}/workdir")
message(STATUS "exported: ${output}")
string(JSON id GET "${output}" id)

run("updating the suite" ${CMAKE_COMMAND} -DFROM=${KIT} -P "${WORK}/project/suite/tools/update_suite.cmake")
if (NOT output MATCHES "suite [0-9.]+ -> [0-9.]+")
    message(FATAL_ERROR "the suite was not updated: ${output}")
endif()

set(tp "${THIRD_PARTY}")
run("configuring the export" ${CMAKE_COMMAND} -S "${WORK}/project" -B "${WORK}/build" -G Ninja
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DCMAKE_C_COMPILER=${CC} -DCMAKE_CXX_COMPILER=${CXX}
    -DSUPPLE_BUILD_VST3=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON
    -DFETCHCONTENT_SOURCE_DIR_CLAP=${tp}/clap
    -DFETCHCONTENT_SOURCE_DIR_CLAP-WRAPPER=${tp}/clap-wrapper
    -DFETCHCONTENT_SOURCE_DIR_DR_LIBS=${tp}/dr_libs/include
    -DFETCHCONTENT_SOURCE_DIR_PUGL=${tp}/pugl
    -DFETCHCONTENT_SOURCE_DIR_NANOVG=${tp}/nanovg
    -DFETCHCONTENT_SOURCE_DIR_NANOSVG=${tp}/nanosvg)
run("building the export" ${CMAKE_COMMAND} --build "${WORK}/build" --parallel 2)
run("the export's own tests" ${CMAKE_CTEST_COMMAND} --test-dir "${WORK}/build" --output-on-failure -j2)
message(STATUS "the export builds and passes its tests")

# The same sound, bit for bit. A home of its own keeps the sandbox from
# scanning the user's plugins.
file(GLOB built "${WORK}/build/plugins/*.clap")
set(home "${WORK}/home")
file(MAKE_DIRECTORY "${home}")
set(args "\"save\": true, \"patch\": {\"params\": ${PATCH}}")
run("the export playing" ${CMAKE_COMMAND} -E env HOME=${home} "${SANDBOX}" exec
    "{\"command\": \"dsp.test\", \"args\": {\"plugin\": \"${id}\", ${args}}}"
    --workdir "${WORK}/export" --plugin-path "${built}")
run("the player playing" ${CMAKE_COMMAND} -E env HOME=${home} "${SANDBOX}" exec
    "{\"command\": \"dsp.test\", \"args\": {\"script\": \"${name}\", ${args}}}"
    --workdir "${WORK}/workdir" --plugin-path "${PLAYER}")
file(GLOB renders "${WORK}/export/renders/*.wav")
if (NOT renders)
    message(FATAL_ERROR "the export rendered nothing")
endif()
foreach(file ${renders})
    get_filename_component(file_name "${file}" NAME)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${file}" "${WORK}/workdir/renders/${file_name}"
                    RESULT_VARIABLE differ)
    if (differ)
        message(FATAL_ERROR "${file_name}: the export and the player play it differently")
    endif()
endforeach()
message(STATUS "the export and the player play the same bytes")
