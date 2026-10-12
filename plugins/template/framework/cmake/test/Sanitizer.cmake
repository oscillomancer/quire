cmake_minimum_required(VERSION 3.27)
foreach(mode address thread realtime)
    execute_process(COMMAND "${CMAKE_COMMAND}" -DMSVC=TRUE "-DSUPPLE_SANITIZE=${mode}"
        -P "${FRAMEWORK}/cmake/CheckSanitizer.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if (result EQUAL 0 OR NOT out MATCHES "not supported with MSVC")
        message(FATAL_ERROR "MSVC sanitizer request was not clearly refused: ${out}")
    endif()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -DMSVC=TRUE -DSUPPLE_SANITIZE=
    -P "${FRAMEWORK}/cmake/CheckSanitizer.cmake" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" -DMSVC=FALSE -DSUPPLE_SANITIZE=address
    -P "${FRAMEWORK}/cmake/CheckSanitizer.cmake" COMMAND_ERROR_IS_FATAL ANY)
