# Sorrel, measured through the built plugin (dsp.test), whose sine peaks at
# -12 dBFS on both channels. Above the threshold it passes unchanged (with
# or without lookahead, which the test compensates); under it, the range
# holds it down, and -80 dB is silence; Expand at 2:1 turns a sine 6 dB
# under the threshold down 6 dB more; a key high-pass keeps a low sine from
# opening it.
# Required: SANDBOX (quire-sandbox), PLUGIN (Sorrel.clap), WORK.
foreach(var SANDBOX PLUGIN WORK)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "response: -D${var}=... is required")
    endif()
endforeach()
# A decimal as an integer number of thousandths (CMake's math has no decimals).
function(milli value out)
    if(value MATCHES "e")
        set(${out} 0 PARENT_SCOPE)  # a tiny value in exponent form
        return()
    endif()
    if(NOT value MATCHES "^(-?)([0-9]+)(\\.([0-9]*))?$")
        message(FATAL_ERROR "not a number: ${value}")
    endif()
    set(sign "${CMAKE_MATCH_1}")
    set(whole "${CMAKE_MATCH_2}")
    string(SUBSTRING "${CMAKE_MATCH_4}000" 0 3 fraction)
    math(EXPR m "${whole} * 1000 + 1${fraction} - 1000")
    if(sign STREQUAL "-")
        math(EXPR m "-${m}")
    endif()
    set(${out} ${m} PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK}")
set(home "${WORK}/home")
file(MAKE_DIRECTORY "${home}")

function(measure params hz out)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"seconds\":2,\"frequency_hz\":${hz},\"signals\":[\"sine\"],\"patch\":{\"params\":${params}}}}"
                --workdir "${WORK}/workdir"
        RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dsp.test failed (${result}) for ${params}:\n${json}\n${err}")
    endif()
    set(${out} "${json}" PARENT_SCOPE)
endfunction()

# The sine's gain and its peak with these settings, which must be within
# 0.01 dB of `want` (a peak under -150 dBFS when want is "silence").
function(expect params hz want)
    measure("${params}" ${hz} json)
    string(JSON gain GET "${json}" signals 0 gain_db)
    string(JSON peak GET "${json}" signals 0 peak_dbfs)
    milli("${peak}" got_peak)
    if(want STREQUAL "silence")
        if(got_peak GREATER -150000)
            message(FATAL_ERROR "${params}: a ${hz} Hz sine should be silent, peaks at ${peak} dBFS")
        endif()
    else()
        milli("${gain}" got)
        milli("${want}" wanted)
        math(EXPR off "${got} - ${wanted}")
        if(off GREATER 10 OR off LESS -10)
            message(FATAL_ERROR "${params}: a ${hz} Hz sine's gain is ${gain} dB, want ${want}")
        endif()
    endif()
    message(STATUS "${params} at ${hz} Hz: gain ${gain} dB, peak ${peak} dBFS")
endfunction()

expect("{}" 1000 0)
expect("{\"lookahead\":\"off\"}" 1000 0)
expect("{\"threshold\":-6}" 1000 -40)
expect("{\"threshold\":-6,\"range\":-80}" 1000 silence)
expect("{\"mode\":\"expand\",\"threshold\":-6,\"ratio\":2}" 1000 -6)
expect("{\"threshold\":-20,\"key_hpf\":2000}" 100 -40)
