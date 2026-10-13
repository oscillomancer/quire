# Source-export conformance and player/native audio equivalence (ADR-0032).
# EXAMPLES runs every example, then SCRIPT/PATCH (the library tilt regression),
# sequentially, each in a fresh project/build. Without EXAMPLES, run one case.
# Required: SANDBOX PLAYER KIT SCRIPT PATCH WORK THIRD_PARTY CC CXX BUILD_TYPE
#           CONFORMANCE GAIN_SCRIPT. CASE is an optional diagnostic name.
foreach(var SANDBOX PLAYER KIT SCRIPT PATCH WORK THIRD_PARTY CC CXX BUILD_TYPE CONFORMANCE GAIN_SCRIPT)
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

if (DEFINED EXAMPLES)
    string(TIMESTAMP started "%s")
    file(REMOVE_RECURSE "${WORK}")
    file(GLOB scripts "${EXAMPLES}/*.cpp")
    list(SORT scripts)
    if (NOT scripts)
        message(FATAL_ERROR "no example scripts in ${EXAMPLES}")
    endif()
    list(APPEND scripts "${SCRIPT}")
    foreach(script IN LISTS scripts)
        get_filename_component(case "${script}" NAME_WE)
        set(patch "{}")
        if (script STREQUAL SCRIPT)
            set(case "library-tilt")
            set(patch "${PATCH}")
        endif()
        run("source export ${case}" ${CMAKE_COMMAND}
            "-DSANDBOX=${SANDBOX}" "-DPLAYER=${PLAYER}" "-DKIT=${KIT}"
            "-DSCRIPT=${script}" "-DPATCH=${patch}" "-DWORK=${WORK}/${case}"
            "-DTHIRD_PARTY=${THIRD_PARTY}" "-DCC=${CC}" "-DCXX=${CXX}"
            "-DBUILD_TYPE=${BUILD_TYPE}" "-DCONFORMANCE=${CONFORMANCE}"
            "-DGAIN_SCRIPT=${GAIN_SCRIPT}" "-DCASE=${case}" -P "${CMAKE_CURRENT_LIST_FILE}")
        message(STATUS "${output}")
    endforeach()
    string(TIMESTAMP finished "%s")
    math(EXPR elapsed "${finished} - ${started}")
    list(LENGTH scripts count)
    message(STATUS "${count} source exports passed in ${elapsed} seconds")
    return()
endif()

get_filename_component(name "${SCRIPT}" NAME_WE)
if (NOT DEFINED CASE)
    set(CASE "${name}")
endif()
file(REMOVE_RECURSE "${WORK}")
file(COPY "${SCRIPT}" DESTINATION "${WORK}/workdir/scripts/${name}")
# A private home prevents scanning the user's plugins, including stale exports.
set(home "${WORK}/home")
file(MAKE_DIRECTORY "${home}")
set(sandbox ${CMAKE_COMMAND} -E env "HOME=${home}" "${SANDBOX}")
run("checking ${CASE}" ${sandbox} exec
    "{\"command\":\"dsp.check\",\"args\":{\"name\":\"${name}\"}}"
    --workdir "${WORK}/workdir")
string(JSON kind GET "${output}" plugin kind)
run("exporting ${CASE}" ${sandbox} exec
    "{\"command\":\"dsp.export\",\"args\":{\"script\":\"${name}\",\"format\":\"source\",\"folder\":\"${WORK}/project\"}}"
    --workdir "${WORK}/workdir")
string(JSON id GET "${output}" id)
string(JSON plugin_name GET "${output}" name)

run("updating ${CASE}'s suite" ${CMAKE_COMMAND} "-DFROM=${KIT}"
    -P "${WORK}/project/suite/tools/update_suite.cmake")
if (NOT output MATCHES "suite [0-9.]+ -> [0-9.]+")
    message(FATAL_ERROR "the suite was not updated: ${output}")
endif()

set(tp "${THIRD_PARTY}")
run("configuring ${CASE}" ${CMAKE_COMMAND} -S "${WORK}/project" -B "${WORK}/build" -G Ninja
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DCMAKE_C_COMPILER=${CC} -DCMAKE_CXX_COMPILER=${CXX}
    -DSUPPLE_BUILD_VST3=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON
    -DFETCHCONTENT_SOURCE_DIR_CLAP=${tp}/clap
    -DFETCHCONTENT_SOURCE_DIR_CLAP-WRAPPER=${tp}/clap-wrapper
    -DFETCHCONTENT_SOURCE_DIR_DR_LIBS=${tp}/dr_libs/include
    -DFETCHCONTENT_SOURCE_DIR_PUGL=${tp}/pugl
    -DFETCHCONTENT_SOURCE_DIR_NANOVG=${tp}/nanovg
    -DFETCHCONTENT_SOURCE_DIR_NANOSVG=${tp}/nanosvg)
run("building ${CASE}" ${CMAKE_COMMAND} --build "${WORK}/build" --parallel 3)
# Keep the original full template-suite regression. Its generic smoke probe
# assumes the first automatable parameter changes neutral DSP, which is not
# true of every example (e.g. bell's Frequency with Gain at 0 dB).
# Every example instead gets the real semantic/audio checks below.
if (CASE STREQUAL "library-tilt")
    run("${CASE}'s own tests" ${CMAKE_CTEST_COMMAND} --test-dir "${WORK}/build" --output-on-failure -j 3)
endif()
set(built "${WORK}/build/plugins/${plugin_name}.clap")
run("${CASE}'s semantic conformance" ${CMAKE_COMMAND} -E env "HOME=${home}"
    "SUPPLE_SCRIPT_LIBRARY=${WORK}/workdir/scripts" "${CONFORMANCE}" "${built}" "${id}")
# Factory-patch checks go here after the coordinator merges that export behavior.

set(patch "{\"params\":${PATCH}}")
if (CASE STREQUAL "sampler" OR CASE STREQUAL "reverb")
    file(COPY "${GAIN_SCRIPT}" DESTINATION "${WORK}/fixtures/scripts/gain")
    if (CASE STREQUAL "sampler")
        set(fixture_signal sine)
        set(property sample)
        set(params "{\"root\":60,\"release\":20,\"level\":-6}")
    else()
        set(fixture_signal impulse)
        set(property room)
        set(params "{\"mix\":100,\"predelay\":0}")
    endif()
    run("generating ${CASE}'s audio file" ${sandbox} exec
        "{\"command\":\"dsp.test\",\"args\":{\"script\":\"gain\",\"signals\":[\"${fixture_signal}\"],\"frequency_hz\":220,\"seconds\":0.5,\"save\":true}}"
        --workdir "${WORK}/fixtures" --plugin-path "${PLAYER}")
    string(JSON asset GET "${output}" signals 0 path)
    set(patch "{\"params\":${params},\"properties\":{\"${property}\":\"${asset}\"}}")
endif()

if (kind STREQUAL "instrument")
    set(signals notes silence)
    set(extra ",\"notes\":[{\"key\":60,\"velocity\":0.8,\"start_s\":0.013,\"length_s\":0.4},{\"key\":67,\"velocity\":0.6,\"start_s\":0.303,\"length_s\":0.4}]")
    set(signal_json "[\"notes\",\"silence\"]")
else()
    set(signals sine sweep impulse noise silence)
    set(extra "")
    set(signal_json "[\"sine\",\"sweep\",\"impulse\",\"noise\",\"silence\"]")
endif()
if (CASE STREQUAL "compressor")
    set(extra ",\"level_db\":-30,\"sidechain\":{\"signal\":\"sine\",\"level_db\":-6,\"start_s\":0.25,\"length_s\":0.5}")
endif()
set(args "\"save\":true,\"seconds\":1,\"signals\":${signal_json},\"patch\":${patch}${extra}")
run("${CASE}'s export playing" ${sandbox} exec
    "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${id}\",${args}}}"
    --workdir "${WORK}/export" --plugin-path "${built}")
set(export_result "${output}")
run("${CASE}'s player playing" ${sandbox} exec
    "{\"command\":\"dsp.test\",\"args\":{\"script\":\"${name}\",${args}}}"
    --workdir "${WORK}/workdir" --plugin-path "${PLAYER}")
set(player_result "${output}")

# These small dsp.test files are canonical stereo float32 RIFF WAVs at 48 kHz,
# with a 44-byte header (audio-file's writer). Validate it before counting frames.
function(wav_frames path out)
    file(READ "${path}" header LIMIT 44 HEX)
    string(LENGTH "${header}" size)
    if (NOT size EQUAL 88)
        message(FATAL_ERROR "${path}: incomplete WAV header")
    endif()
    string(SUBSTRING "${header}" 0 8 riff)
    string(SUBSTRING "${header}" 16 64 layout)
    if (NOT riff STREQUAL "52494646" OR
        NOT layout STREQUAL "57415645666d7420100000000300020080bb000000dc05000800200064617461")
        message(FATAL_ERROR "${path}: expected canonical stereo float32 48 kHz WAV")
    endif()
    set(data_bytes 0)
    foreach(i RANGE 0 3)
        math(EXPR offset "80 + 2 * ${i}")
        string(SUBSTRING "${header}" ${offset} 2 byte)
        math(EXPR data_bytes "${data_bytes} + (0x${byte} << (8 * ${i}))")
    endforeach()
    file(SIZE "${path}" bytes)
    math(EXPR payload "${bytes} - 44")
    math(EXPR remainder "${payload} % 8")
    if (NOT payload EQUAL data_bytes OR NOT remainder EQUAL 0)
        message(FATAL_ERROR "${path}: truncated WAV or incomplete stereo frame")
    endif()
    math(EXPR frames "${payload} / 8")
    set(${out} "${frames}" PARENT_SCOPE)
endfunction()

list(LENGTH signals expected_count)
foreach(mode export player)
    string(JSON count LENGTH "${${mode}_result}" signals)
    if (NOT count EQUAL expected_count)
        message(FATAL_ERROR "${CASE} ${mode}: expected ${expected_count} signals, got ${count}")
    endif()
endforeach()
set(index 0)
foreach(signal IN LISTS signals)
    foreach(mode export player)
        string(JSON actual GET "${${mode}_result}" signals ${index} signal)
        if (NOT actual STREQUAL signal)
            message(FATAL_ERROR "${CASE} ${mode}: expected ${signal}, got ${actual}")
        endif()
        string(JSON path GET "${${mode}_result}" signals ${index} path)
        wav_frames("${path}" ${mode}_frames)
        if (${mode}_frames LESS 48000)
            message(FATAL_ERROR "${CASE} ${mode} ${signal}: render shorter than its one-second input")
        endif()
        set(${mode}_path "${path}")
        if ((CASE STREQUAL "sampler" AND signal STREQUAL "notes") OR
            (CASE STREQUAL "reverb" AND signal STREQUAL "impulse"))
            string(JSON peak GET "${${mode}_result}" signals ${index} peak_dbfs)
            if (peak LESS -100)
                message(FATAL_ERROR "${CASE} ${mode}: loaded audio file produced no audible output")
            endif()
        endif()
        if (CASE STREQUAL "compressor" AND signal STREQUAL "sine")
            string(JSON duck GET "${${mode}_result}" signals ${index} sidechain gain_change_db)
            if (duck STREQUAL "" OR duck GREATER -3)
                message(FATAL_ERROR "${CASE} ${mode}: timed key did not duck the main sine")
            endif()
        endif()
    endforeach()
    if (NOT export_frames EQUAL player_frames)
        message(FATAL_ERROR "${CASE} ${signal}: frame counts differ (${export_frames} vs ${player_frames})")
    endif()
    run("${CASE} ${signal}: comparing ${export_frames} frames" ${CMAKE_COMMAND} -E compare_files
        "${export_path}" "${player_path}")
    math(EXPR index "${index} + 1")
endforeach()
message(STATUS "${CASE}: semantic conformance, ${expected_count} signals/frame counts/bytes passed")
