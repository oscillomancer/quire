# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Replaces this product's suite/ with a newer one, so a fix made in the
# suite reaches a source export unchanged. FROM is a folder with the newer
# suite/ in it: a newer source export, or the export kit a newer script host
# stages (plugins/script-export in its build). Run from the product's root:
#
#   cmake -DFROM=../newer-export -P suite/tools/update_suite.cmake
#
# suite/ is not edited inside a product; product/ (the script, its keys and
# the product's own files) is the product's, and this leaves it alone.
#
# Afterwards: delete the build directories and run every preset's tests.

cmake_minimum_required(VERSION 3.27)

if (NOT FROM)
    message(FATAL_ERROR "update_suite: -DFROM=<a folder with the newer suite/> is required")
endif()
# Real paths: a FROM reached through a symlink must still be recognised as
# this very suite, or the copy below would delete its own source.
get_filename_component(from "${FROM}/suite" REALPATH)
get_filename_component(here "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
if (NOT EXISTS "${from}/VERSION.txt" OR NOT EXISTS "${from}/suite.cmake")
    message(FATAL_ERROR "update_suite: ${from} is not a suite directory")
endif()
string(FIND "${from}/" "${here}/" from_in_here)
string(FIND "${here}/" "${from}/" here_in_from)
if (from_in_here EQUAL 0 OR here_in_from EQUAL 0)
    message(FATAL_ERROR "update_suite: ${from} and ${here} are the same suite or nested")
endif()
if (NOT EXISTS "${here}/../product/product.cmake")
    message(FATAL_ERROR "update_suite: run this from a product with the suite in suite/")
endif()

set(old "unknown")
if (EXISTS "${here}/VERSION.txt")
    file(READ "${here}/VERSION.txt" old)
endif()
file(READ "${from}/VERSION.txt" new)
string(STRIP "${old}" old)
string(STRIP "${new}" new)

# Copy beside the old suite first and swap only once the copy is complete,
# so a failed or interrupted copy leaves the product as it was.
get_filename_component(root "${here}/.." ABSOLUTE)
set(staging "${root}/.suite-update")
set(previous "${root}/.suite-previous")
foreach(scratch "${staging}" "${previous}")
    get_filename_component(scratch_real "${scratch}" REALPATH)
    string(FIND "${from}/" "${scratch_real}/" source_in_scratch)
    string(FIND "${scratch_real}/" "${from}/" scratch_in_source)
    if (source_in_scratch EQUAL 0 OR scratch_in_source EQUAL 0)
        message(FATAL_ERROR "update_suite: ${from} overlaps the staging directory ${scratch}")
    endif()
endforeach()
file(REMOVE_RECURSE "${staging}" "${previous}")
file(MAKE_DIRECTORY "${staging}")
file(COPY "${from}/" DESTINATION "${staging}")
if (NOT EXISTS "${staging}/VERSION.txt" OR NOT EXISTS "${staging}/suite.cmake")
    file(REMOVE_RECURSE "${staging}")
    message(FATAL_ERROR "update_suite: the copy is incomplete; suite/ is unchanged")
endif()
file(RENAME "${here}" "${previous}")
file(RENAME "${staging}" "${here}")
file(REMOVE_RECURSE "${previous}")
message(STATUS "suite ${old} -> ${new}")
message(STATUS "Delete build/, and run every preset's tests.")
