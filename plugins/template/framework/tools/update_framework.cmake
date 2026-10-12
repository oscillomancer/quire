# Replaces this product's framework/ with the template's, so a fix made in
# the template reaches the product unchanged. Run from the product's root:
#
#   cmake -DFROM=../template -P framework/tools/update_framework.cmake
#
# framework/ is never edited inside a product: it is the template's,
# byte for byte. A product that needs a framework change makes it in the
# template, tests it there, and brings it over with this.
#
# It replaces framework/ and nothing else. When a framework change needs an
# edit outside framework/, framework/CHANGES.md says what to change and the
# product's owner makes it by hand.
#
# Afterwards: read framework/CHANGES.md for anything a product must do,
# delete the build directories, and run every preset's tests.

cmake_minimum_required(VERSION 3.27)

if (NOT FROM)
    message(FATAL_ERROR "update_framework: -DFROM=<path to the template> is required")
endif()
# Real paths: a FROM reached through a symlink must still be recognised as
# this very framework, or the copy below would delete its own source.
get_filename_component(from "${FROM}/framework" REALPATH)
get_filename_component(here "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
if (NOT EXISTS "${from}/VERSION.txt")
    message(FATAL_ERROR "update_framework: ${from} is not a framework directory")
endif()
string(FIND "${from}/" "${here}/" from_in_here)
string(FIND "${here}/" "${from}/" here_in_from)
if (from_in_here EQUAL 0 OR here_in_from EQUAL 0)
    message(FATAL_ERROR "update_framework: ${from} and ${here} are the same framework or nested")
endif()
if (NOT EXISTS "${here}/../product/product.cmake")
    message(FATAL_ERROR "update_framework: run this from a product built on the framework")
endif()

set(old "unknown")
if (EXISTS "${here}/VERSION.txt")
    file(READ "${here}/VERSION.txt" old)
endif()
file(READ "${from}/VERSION.txt" new)
string(STRIP "${old}" old)
string(STRIP "${new}" new)

# Copy beside the old framework first and swap only once the copy is
# complete, so a failed or interrupted copy leaves the product as it was.
get_filename_component(root "${here}/.." ABSOLUTE)
set(staging "${root}/.framework-update")
set(previous "${root}/.framework-previous")
foreach(scratch "${staging}" "${previous}")
    get_filename_component(scratch_real "${scratch}" REALPATH)
    string(FIND "${from}/" "${scratch_real}/" source_in_scratch)
    string(FIND "${scratch_real}/" "${from}/" scratch_in_source)
    if (source_in_scratch EQUAL 0 OR scratch_in_source EQUAL 0)
        message(FATAL_ERROR "update_framework: ${from} overlaps the staging directory ${scratch}")
    endif()
endforeach()
file(REMOVE_RECURSE "${staging}" "${previous}")
file(MAKE_DIRECTORY "${staging}")
file(COPY "${from}/" DESTINATION "${staging}")
if (NOT EXISTS "${staging}/VERSION.txt" OR NOT EXISTS "${staging}/cmake/Framework.cmake")
    file(REMOVE_RECURSE "${staging}")
    message(FATAL_ERROR "update_framework: the copy is incomplete; framework/ is unchanged")
endif()
file(RENAME "${here}" "${previous}")
file(RENAME "${staging}" "${here}")
file(REMOVE_RECURSE "${previous}")
message(STATUS "framework ${old} -> ${new}")
message(STATUS "Read framework/CHANGES.md, delete build/, and run every preset's tests.")
