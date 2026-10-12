# The validators a release goes through, run here as CI runs them:
#
#   cmake -DPRESET=release -P framework/tools/validate.cmake
#
# builds the preset, then runs
#   - pluginval (Tracktion), strictness 8, in process, on the VST3 -- and on
#     macOS the AU -- fetched at a pinned version (checksummed) into
#     build/tools unless PLUGINVAL names one or one is on the PATH;
#   - auval -strict on macOS, with the product's own type and codes;
#   - clap-validator, when it is on the PATH or the build found it.
# On macOS the AU has to be installed to be found, so the formats are
# installed first (product-install); elsewhere the build's own files are
# validated where they are. Without a display on Linux, pluginval skips its
# editor tests and says so. Exits non-zero if any validator failed.
cmake_minimum_required(VERSION 3.27)

if (NOT PRESET)
    set(PRESET release)
endif()
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(build "${root}/build/${PRESET}")
set(pluginval_version 1.0.4)

function(step name)
    message(STATUS "-- ${name}")
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result)
    if (result EQUAL 0)
        set_property(GLOBAL APPEND PROPERTY passed "${name}")
    else()
        set_property(GLOBAL APPEND PROPERTY failed "${name}")
    endif()
endfunction()

execute_process(COMMAND ${CMAKE_COMMAND} --preset ${PRESET} WORKING_DIRECTORY "${root}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND ${CMAKE_COMMAND} --build --preset ${PRESET} WORKING_DIRECTORY "${root}" COMMAND_ERROR_IS_FATAL ANY)
include("${build}/supple-build.cmake")
if (BUILD_SANITIZE)
    message(FATAL_ERROR "validate: ${PRESET} is a sanitizer build; validators load the plugin into their own process")
endif()
if (APPLE)
    execute_process(COMMAND ${CMAKE_COMMAND} --build "${build}" --target product-install COMMAND_ERROR_IS_FATAL ANY)
    set(vst3 "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/${BUILD_NAME}.vst3")
    set(component "$ENV{HOME}/Library/Audio/Plug-Ins/Components/${BUILD_NAME}.component")
else()
    set(vst3 "${BUILD_VST3}")
endif()

# --- pluginval ----------------------------------------------------------------------

if (NOT PLUGINVAL)
    find_program(PLUGINVAL pluginval)
endif()
if (NOT PLUGINVAL)
    if (APPLE)
        set(asset pluginval_macOS.zip)
        set(hash 3c4c533bda0c5059eea3ddaea752d757ee2025041f0f47e6bcb0e87f6082b29f)
        set(exe pluginval.app/Contents/MacOS/pluginval)
    elseif (WIN32)
        set(asset pluginval_Windows.zip)
        set(hash c08e61ce3b96db41636f8ec7e76f4c7e2c13ebdac7fa1b5a1f52b4f32ec715ab)
        set(exe pluginval.exe)
    else()
        set(asset pluginval_Linux.zip)
        set(hash c01c49d8063965c4c2dea8324468336768f5c9139e0b1caebde14c2400b55352)
        set(exe pluginval)
    endif()
    set(tools "${root}/build/tools/pluginval-${pluginval_version}")
    if (NOT EXISTS "${tools}/${exe}")
        file(DOWNLOAD "https://github.com/Tracktion/pluginval/releases/download/v${pluginval_version}/${asset}"
            "${tools}/${asset}" EXPECTED_HASH SHA256=${hash} STATUS status)
        list(GET status 0 code)
        if (NOT code EQUAL 0)
            message(FATAL_ERROR "validate: could not fetch pluginval ${pluginval_version}: ${status}")
        endif()
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf "${asset}" WORKING_DIRECTORY "${tools}" COMMAND_ERROR_IS_FATAL ANY)
        file(CHMOD "${tools}/${exe}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
    endif()
    set(PLUGINVAL "${tools}/${exe}")
endif()

set(pluginval_options --strictness-level 8 --validate-in-process --timeout-ms 300000)
if (UNIX AND NOT APPLE AND "$ENV{DISPLAY}" STREQUAL "" AND "$ENV{WAYLAND_DISPLAY}" STREQUAL "")
    message(STATUS "No display: pluginval skips its editor tests")
    list(APPEND pluginval_options --skip-gui-tests)
endif()
step("pluginval VST3" "${PLUGINVAL}" ${pluginval_options} --validate "${vst3}")
if (APPLE)
    step("pluginval AU" "${PLUGINVAL}" ${pluginval_options} --validate "${component}")
    step("auval" auval -strict -v ${BUILD_AU_TYPE} ${BUILD_AU_SUBTYPE} ${BUILD_AU_MANUFACTURER})
endif()

# --- clap-validator -------------------------------------------------------------------

file(STRINGS "${build}/CMakeCache.txt" cached REGEX "^SUPPLE_CLAP_VALIDATOR:")
string(REGEX REPLACE "^[^=]*=" "" clap_validator "${cached}")
if (NOT clap_validator OR NOT EXISTS "${clap_validator}")
    find_program(clap_validator clap-validator)
endif()
if (clap_validator)
    set(options --only-failed)
    if (BUILD_PRESETS)
        # As Tests.cmake explains: 0.4.1 deadlocks on a second factory preset.
        list(APPEND options --exclude "^preset-discovery-(crawl|load)$")
    endif()
    step("clap-validator" "${clap_validator}" validate ${options} "${BUILD_CLAP}")
else()
    message(STATUS "clap-validator not found (cargo install --git https://github.com/free-audio/clap-validator)")
endif()

get_property(passed GLOBAL PROPERTY passed)
get_property(failed GLOBAL PROPERTY failed)
foreach(name ${passed})
    message(STATUS "passed: ${name}")
endforeach()
foreach(name ${failed})
    message(STATUS "FAILED: ${name}")
endforeach()
if (failed)
    message(FATAL_ERROR "validate: a validator failed")
endif()
