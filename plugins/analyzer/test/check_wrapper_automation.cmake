if (NOT DEFINED WRAPPER_FILE OR NOT EXISTS "${WRAPPER_FILE}")
    message(FATAL_ERROR "clap-wrapper VST3 process source not found: ${WRAPPER_FILE}")
endif()

file(READ "${WRAPPER_FILE}" source)
string(FIND "${source}" "getPoint(nums - 1" last_point_only)
if (NOT last_point_only EQUAL -1)
    message(FATAL_ERROR "clap-wrapper still collapses VST3 automation to the final point")
endif()

string(FIND "${source}" "for (decltype(nums) j = 0; j < nums; ++j)" every_point_loop)
if (every_point_loop EQUAL -1)
    message(FATAL_ERROR "clap-wrapper does not iterate every VST3 automation point")
endif()
