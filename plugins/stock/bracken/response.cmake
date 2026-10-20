# Bracken, measured through the built plugin (dsp.test), whose sine peaks at
# -12 dBFS on both channels. Dry (mix 0) it passes unchanged; a single echo
# a millisecond late, alone, is the sine at its own level; ducking takes 12
# dB off the echoes while the sine plays; output trims what leaves. The
# echo time and decay readouts agree with the time and feedback as set.
# Required: SANDBOX (quire-sandbox), PLUGIN (Bracken.clap), WORK.
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

function(measure params out)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"seconds\":2,\"frequency_hz\":1000,\"level_db\":-12,\"signals\":[\"sine\"],\"patch\":{\"params\":${params}}}}"
                --workdir "${WORK}/workdir"
        RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dsp.test failed (${result}) for ${params}:\n${json}\n${err}")
    endif()
    set(${out} "${json}" PARENT_SCOPE)
endfunction()

# The sine's gain with these settings, which must be within 0.01 dB of `want`.
function(expect params want)
    measure("${params}" json)
    string(JSON gain GET "${json}" signals 0 gain_db)
    milli("${gain}" got)
    milli("${want}" wanted)
    math(EXPR off "${got} - ${wanted}")
    if(off GREATER 10 OR off LESS -10)
        message(FATAL_ERROR "${params}: a 1 kHz sine's gain is ${gain} dB, want ${want}")
    endif()
    message(STATUS "${params}: gain ${gain} dB")
endfunction()

expect("{\"mix\":0}" 0)
expect("{\"mix\":0,\"output\":-6}" -6)
expect("{\"sync\":\"free\",\"time\":1,\"feedback\":0,\"mix\":100}" 0)
expect("{\"sync\":\"free\",\"time\":1,\"feedback\":0,\"mix\":100,\"duck\":12}" -12)

# A readout, within `tolerance` thousandths of `want`.
function(expect_meter params key want tolerance)
    measure("${params}" json)
    string(JSON value GET "${json}" signals 0 meters ${key})
    milli("${value}" got)
    milli("${want}" wanted)
    math(EXPR off "${got} - ${wanted}")
    if(off GREATER ${tolerance} OR off LESS -${tolerance})
        message(FATAL_ERROR "${params}: ${key} reads ${value}, want ${want}")
    endif()
    message(STATUS "${params}: ${key} ${value}")
endfunction()

# A dotted eighth at 120 BPM (the tempo when the host sends none).
expect_meter("{\"sync\":\"n8d\"}" echo_time 375 1)
# Each echo 6.02 dB below the last, every 250 ms: 60 dB in 2.491 s.
expect_meter(
    "{\"sync\":\"free\",\"time\":250,\"feedback\":50,\"low_cut\":20,\"high_cut\":20000,\"age\":0}"
    decay 2491.4 1000)
