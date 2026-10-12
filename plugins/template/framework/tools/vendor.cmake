# Copies every pinned third-party source (cmake/Pins.cmake) into one folder,
# for builds with no network: an air-gapped machine, an archive of a
# release's exact sources, a CI cache.
#
#   cmake -P framework/tools/vendor.cmake                 # into vendor/ at the root
#   cmake -DDIR=/path/to/folder -P framework/tools/vendor.cmake
#
# A configure finds vendor/ at the root by itself; any other folder is given
# with -DSUPPLE_VENDOR_DIR=/path/to/folder. Everything is fetched, for every
# platform and option, so one folder builds anything. The checkouts carry no
# Git history (commit vendor/ to the product's repository, or keep it
# outside), and each records its pin: after a framework update that moves a
# pin, run this again -- it fetches only what changed, and a configure stops
# until it has. Patches are never applied here; a build applies them to its
# own copy.
cmake_minimum_required(VERSION 3.27)

get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include(${root}/framework/cmake/Pins.cmake)
if (NOT DIR)
    set(DIR "${root}/vendor")
endif()
get_filename_component(dir "${DIR}" ABSOLUTE)
find_package(Git REQUIRED)

function(run)
    execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${checkout}" RESULT_VARIABLE result
        OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if (NOT result EQUAL 0)
        string(REPLACE ";" " " command "${ARGN}")
        message(FATAL_ERROR "vendor: ${command} failed in ${checkout}:\n${out}")
    endif()
endfunction()

file(MAKE_DIRECTORY "${dir}")
foreach(name ${SUPPLE_PINS})
    set(checkout "${dir}/${name}")
    supple_pin_stamp(${name} want)
    set(have "")
    if (EXISTS "${checkout}/.supple-pin")
        file(READ "${checkout}/.supple-pin" have)
        string(STRIP "${have}" have)
    endif()
    if (have STREQUAL want)
        message(STATUS "${name}: up to date")
        continue()
    endif()
    list(GET SUPPLE_PIN_${name} 0 url)
    list(GET SUPPLE_PIN_${name} 1 ref)
    message(STATUS "${name}: fetching ${ref}")
    file(REMOVE_RECURSE "${checkout}")
    file(MAKE_DIRECTORY "${checkout}")
    # One commit, no history: a fetch by commit or tag. A named remote, for
    # submodules given relative to it (the VST3 SDK's).
    run(${GIT_EXECUTABLE} init -q)
    run(${GIT_EXECUTABLE} remote add origin "${url}")
    run(${GIT_EXECUTABLE} fetch -q --depth 1 origin "${ref}")
    run(${GIT_EXECUTABLE} -c advice.detachedHead=false checkout -q FETCH_HEAD)
    if (SUPPLE_PIN_${name}_MODULES)
        run(${GIT_EXECUTABLE} submodule update -q --init --depth 1 -- ${SUPPLE_PIN_${name}_MODULES})
    endif()
    # Every .git, the submodules' files included: the folder is plain files.
    file(GLOB_RECURSE gits LIST_DIRECTORIES true "${checkout}/*/.git")
    list(APPEND gits "${checkout}/.git")
    foreach(git ${gits})
        get_filename_component(leaf "${git}" NAME)
        if (leaf STREQUAL ".git")
            file(REMOVE_RECURSE "${git}")
        endif()
    endforeach()
    file(WRITE "${checkout}/.supple-pin" "${want}\n")
endforeach()

# Anything left from an older pin list.
file(GLOB entries LIST_DIRECTORIES true RELATIVE "${dir}" "${dir}/*")
foreach(entry ${entries})
    if (IS_DIRECTORY "${dir}/${entry}" AND NOT entry IN_LIST SUPPLE_PINS)
        message(STATUS "${entry}: no longer pinned; removed")
        file(REMOVE_RECURSE "${dir}/${entry}")
    endif()
endforeach()

file(READ "${root}/framework/VERSION.txt" version)
string(STRIP "${version}" version)
file(WRITE "${dir}/.supple-vendor" "framework ${version}\n")
file(WRITE "${dir}/README.txt" "Third-party sources for an offline build, at the pins in
framework/cmake/Pins.cmake (framework ${version}). Made by
framework/tools/vendor.cmake; each folder's .supple-pin names its repository
and commit. Their licences are in each folder and summarised in
framework/NOTICES.txt.
")
message(STATUS "Vendored into ${dir}")
