# The VST3 wrapper clamps every register_timer() request to a floor. Upstream
# sets that floor at 30ms, which caps the editor at 33fps no matter what the
# plugin asks for; the patch lowers it to 8ms so a 16ms request survives. If
# the patch stops applying, the editor silently drops to 33fps and nothing
# else fails -- so check it here rather than notice it by eye.
if (NOT DEFINED WRAPPER_FILE OR NOT EXISTS "${WRAPPER_FILE}")
    message(FATAL_ERROR "clap-wrapper VST3 source not found: ${WRAPPER_FILE}")
endif()

file(READ "${WRAPPER_FILE}" source)

string(FIND "${source}" "period_ms = 30" thirty_ms_floor)
if (NOT thirty_ms_floor EQUAL -1)
    message(FATAL_ERROR "clap-wrapper still clamps the VST3 timer to 30ms (33fps)")
endif()

string(FIND "${source}" "period_ms = 8" eight_ms_floor)
if (eight_ms_floor EQUAL -1)
    message(FATAL_ERROR "clap-wrapper VST3 timer floor patch is not applied")
endif()
