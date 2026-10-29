# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Pollard plays each pad with Coppice's engine: the text between the
# "engine: shared with Pollard" marker and "end of the engine" in Coppice's
# script, copied into Pollard's, since a script cannot include another. Each
# compiles on its own, so a copy that drifts shows only as the two playing a
# sound differently. This checks the two hold the same engine, byte for byte.
# Required: COPPICE, POLLARD (the two scripts).
foreach(var COPPICE POLLARD)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "engine-copy: -D${var}=... is required")
    endif()
endforeach()

# The engine in `file`, from its first marker to the end of its last line.
function(engine file out)
    file(READ "${file}" text)
    string(FIND "${text}" "// --- engine: shared with Pollard, byte for byte" from)
    string(FIND "${text}" "// --- end of the engine" to)
    if(from LESS 0 OR to LESS from)
        message(FATAL_ERROR "${file} has no engine between the markers \"// --- engine: shared "
                            "with Pollard, byte for byte\" and \"// --- end of the engine\"")
    endif()
    math(EXPR length "${to} - ${from}")
    string(SUBSTRING "${text}" ${from} ${length} region)
    set(${out} "${region}" PARENT_SCOPE)
endfunction()

engine("${COPPICE}" coppice)
engine("${POLLARD}" pollard)
if(NOT coppice STREQUAL pollard)
    string(SHA256 a "${coppice}")
    string(SHA256 b "${pollard}")
    message(FATAL_ERROR "the engines in ${COPPICE} and ${POLLARD} differ (${a} against ${b}): "
                        "make the change in both, byte for byte")
endif()
message(STATUS "the engine is the same in both scripts")
