# Creates a new product from this template.
#
#   cmake -DNAME="Loupe" -DID=com.suppleaudio.loupe -DCODE=LoUp -DDEST=../loupe \
#         [-DKIND=effect|instrument|note-effect | -DEXAMPLE=duck] [-DDESCRIPTION="A parametric EQ."] \
#         [-DLICENSE=GPL-3.0-or-later|MIT] \
#         -P framework/tools/new_product.cmake
#
# NAME is the display name. ID is the permanent reverse-DNS identifier. CODE
# is the permanent four-character AU subtype, also used as the state tag.
# The starting point is the template's effect, or with EXAMPLE any directory
# in examples/ (KIND=instrument is EXAMPLE=instrument, KIND=note-effect is
# EXAMPLE=chord). LICENSE
# defaults to GPL-3.0-or-later, which Supple Audio products use; the
# framework/ directory stays MIT either way.
#
# The result builds and passes every test as it stands. Then: describe the
# product in product/, starting with Params.h and Processor.h.

cmake_minimum_required(VERSION 3.27)

get_filename_component(template "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

foreach(var NAME ID CODE DEST)
    if (NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "new_product: -D${var}=... is required (see the top of this file)")
    endif()
endforeach()
if (KIND AND EXAMPLE)
    message(FATAL_ERROR "new_product: give KIND or EXAMPLE, not both")
endif()
if (NOT KIND)
    set(KIND effect)
endif()
if (NOT LICENSE)
    set(LICENSE GPL-3.0-or-later)
endif()
if (NOT KIND MATCHES "^(effect|instrument|note-effect)$")
    message(FATAL_ERROR "new_product: KIND must be effect, instrument or note-effect")
endif()
if (KIND STREQUAL "instrument")
    set(EXAMPLE instrument)
elseif (KIND STREQUAL "note-effect")
    set(EXAMPLE chord)
endif()
file(GLOB example_dirs LIST_DIRECTORIES true "${template}/examples/*")
set(examples "")
set(reserved_codes "")
set(reserved_names "")
set(reserved_ids "")
foreach(dir "${template}/product" ${example_dirs})
    if (IS_DIRECTORY "${dir}" AND EXISTS "${dir}/product.cmake")
        if (NOT dir STREQUAL "${template}/product")
            get_filename_component(name "${dir}" NAME)
            list(APPEND examples ${name})
        endif()
        file(STRINGS "${dir}/product.cmake" identity_lines REGEX "^set\\(PRODUCT_(NAME|ID|AU_SUBTYPE|STATE_TAG) ")
        foreach(line IN LISTS identity_lines)
            string(REGEX MATCH "set\\(PRODUCT_([A-Z_]+) +\"([^\"]*)\"" _ "${line}")
            set(field "${CMAKE_MATCH_1}")
            set(value "${CMAKE_MATCH_2}")
            if (field STREQUAL "NAME")
                string(TOLOWER "${value}" value)
                list(APPEND reserved_names "${value}")
            elseif (field STREQUAL "ID")
                list(APPEND reserved_ids "${value}")
            else()
                list(APPEND reserved_codes "${value}")
            endif()
        endforeach()
    endif()
endforeach()
if (EXAMPLE AND NOT EXAMPLE IN_LIST examples)
    list(JOIN examples ", " known)
    message(FATAL_ERROR "new_product: no example called ${EXAMPLE} (there are: ${known})")
endif()
if (NOT LICENSE MATCHES "^(GPL-3.0-or-later|MIT)$")
    message(FATAL_ERROR "new_product: LICENSE must be GPL-3.0-or-later or MIT")
endif()
# These land inside quoted CMake strings and C++ string literals: nothing
# that either would interpret.
foreach(text NAME DESCRIPTION)
    if ("${${text}}" MATCHES "[\"\\\\$;@\n\r]")
        message(FATAL_ERROR "new_product: ${text} may not contain quotes, backslashes, $, ;, @ or line breaks")
    endif()
endforeach()
# The display name is also a filename on every supported platform.
if (NAME MATCHES "[/\\\\:*?\"<>|]")
    message(FATAL_ERROR "new_product: NAME contains a character forbidden in portable filenames")
endif()
foreach(code RANGE 1 31)
    string(ASCII ${code} control)
    string(FIND "${NAME}" "${control}" found)
    if (NOT found EQUAL -1)
        message(FATAL_ERROR "new_product: NAME may not contain control characters")
    endif()
endforeach()
string(ASCII 127 delete)
string(FIND "${NAME}" "${delete}" found)
if (NOT found EQUAL -1)
    message(FATAL_ERROR "new_product: NAME may not contain control characters")
endif()
# Windows drops a trailing dot or space from a filename, and takes a device
# name before the first dot (NUL.clap, Com1.vst3) for the device.
if (NAME MATCHES "[. ]$")
    message(FATAL_ERROR "new_product: NAME may not end in a dot or a space")
endif()
string(REGEX REPLACE "\\..*$" "" device "${NAME}")
string(REGEX REPLACE " +$" "" device "${device}")
string(TOUPPER "${device}" device)
if (device MATCHES "^(CON|PRN|AUX|NUL|(COM|LPT)([0-9]|¹|²|³))$")
    message(FATAL_ERROR "new_product: NAME is a Windows device name (CON, PRN, AUX, NUL, COM1, LPT1...)")
endif()
if (NOT ID MATCHES "^[A-Za-z0-9-]+(\\.[A-Za-z0-9-]+)+$")
    message(FATAL_ERROR "new_product: ID must be reverse-DNS, e.g. com.suppleaudio.product")
endif()
if (NOT CODE MATCHES "^[A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9]$")
    message(FATAL_ERROR "new_product: CODE must be exactly four letters or digits")
endif()
string(TOLOWER "${NAME}" lower_name)
if (NOT ALLOW_EXAMPLE_CODE)
    if (CODE IN_LIST reserved_codes OR ID IN_LIST reserved_ids OR lower_name IN_LIST reserved_names)
        message(FATAL_ERROR "new_product: NAME, ID and CODE must not reuse the template's or examples' identities")
    endif()
endif()

get_filename_component(dest "${DEST}" ABSOLUTE)
if (EXISTS "${dest}")
    file(GLOB existing "${dest}/*" "${dest}/.*")
    if (existing)
        message(FATAL_ERROR "new_product: ${dest} exists and is not empty")
    endif()
endif()
file(MAKE_DIRECTORY "${dest}")

string(TIMESTAMP year "%Y")
file(READ "${template}/framework/VERSION.txt" framework_version)
string(STRIP "${framework_version}" framework_version)

# --- copy ------------------------------------------------------------------------------

file(COPY "${template}/framework" DESTINATION "${dest}")
foreach(file CMakeLists.txt CMakePresets.json .gitignore)
    file(COPY "${template}/${file}" DESTINATION "${dest}")
endforeach()
if (EXAMPLE)
    file(COPY "${template}/examples/${EXAMPLE}/" DESTINATION "${dest}/product")
else()
    file(COPY "${template}/product/" DESTINATION "${dest}/product")
endif()
# What the source product cost on its author's machines is no baseline for a
# new one; it records its own (tools/benchmark.cpp).
file(REMOVE "${dest}/product/benchmark.txt")

# --- identity ----------------------------------------------------------------------------

set(product_cmake "${dest}/product/product.cmake")
file(READ "${product_cmake}" text)
function(set_line var value)
    string(REGEX REPLACE "set\\(${var}( +)\"[^\"]*\"\\)" "set(${var}\\1\"${value}\")" text "${text}")
    set(text "${text}" PARENT_SCOPE)
endfunction()
set_line(PRODUCT_NAME "${NAME}")
set_line(PRODUCT_ID "${ID}")
set_line(PRODUCT_VERSION "0.1.0")
set_line(PRODUCT_AU_SUBTYPE "${CODE}")
set_line(PRODUCT_STATE_TAG "${CODE}")
if (DESCRIPTION)
    set_line(PRODUCT_DESCRIPTION "${DESCRIPTION}")
else()
    set(DESCRIPTION "")
endif()
if (LICENSE STREQUAL "GPL-3.0-or-later")
    set_line(PRODUCT_COPYRIGHT "Copyright (C) ${year} Supple Audio")
else()
    set_line(PRODUCT_COPYRIGHT "Copyright (c) ${year} Supple Audio")
endif()
file(WRITE "${product_cmake}" "${text}")

# --- license -------------------------------------------------------------------------------

if (LICENSE STREQUAL "GPL-3.0-or-later")
    file(COPY_FILE "${template}/framework/tools/licenses/GPL-3.0-or-later.txt" "${dest}/LICENSE")
    # The product's own sources take the product's license; framework/ keeps
    # its MIT headers, which is both correct and what keeps it byte-identical
    # with the template for updates.
    file(GLOB_RECURSE product_sources "${dest}/product/*.h" "${dest}/product/*.cpp" "${dest}/product/*.cmake")
    foreach(source ${product_sources})
        file(READ "${source}" content)
        string(REPLACE "SPDX-License-Identifier: MIT" "SPDX-License-Identifier: GPL-3.0-or-later" content "${content}")
        string(REGEX REPLACE "Copyright \\(c\\) [0-9]+ Supple Audio" "Copyright (C) ${year} Supple Audio" content "${content}")
        file(WRITE "${source}" "${content}")
    endforeach()
    set(license_text "${NAME} is free software, licensed under the GNU General Public License version 3 or (at your option) any later version; see [LICENSE](LICENSE).")
else()
    file(READ "${template}/LICENSE" mit)
    string(REGEX REPLACE "Copyright \\(c\\) [0-9]+ Supple Audio" "Copyright (c) ${year} Supple Audio" mit "${mit}")
    file(WRITE "${dest}/LICENSE" "${mit}")
    set(license_text "${NAME} is MIT licensed; see [LICENSE](LICENSE).")
endif()

# --- product documents -----------------------------------------------------------------------

file(WRITE "${dest}/README.md" "# ${NAME}

${DESCRIPTION}

Built on the Supple Audio plugin framework ${framework_version}. Everything that
makes this product what it is lives in `product/`; `framework/` is shared,
unchanged, with every product, and is updated from the template with
`framework/tools/update_framework.cmake`.

## Building

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

See [framework/docs/BUILDING.md](framework/docs/BUILDING.md) for platforms,
sanitizers, validators and installing.

## License

${license_text} The framework in `framework/` is MIT licensed
([framework/LICENSE](framework/LICENSE)). Third-party notices are in
[framework/NOTICES.txt](framework/NOTICES.txt) and ship inside every built
plugin.
")

file(WRITE "${dest}/AGENTS.md" "# ${NAME}: instructions for agents

Read [framework/AGENTS.md](framework/AGENTS.md) first. It is the contract for
every product built on this framework: where code goes, what may not be
changed, and what \"done\" means.

## This product

Keep this section current as the product takes shape: what the DSP does and
why, each parameter's meaning, latency and tail behaviour, anything that
needs a restart, and every measured claim with the test that measures it.
")

if (EXAMPLE)
    set(start "the ${EXAMPLE} example")
else()
    set(start "the starter effect")
endif()
message(STATUS "Created ${NAME} (${ID}, ${CODE}, from ${start}, ${LICENSE}) at ${dest}")
message(STATUS "Next:")
message(STATUS "  cd ${dest} && git init")
message(STATUS "  cmake --preset release && cmake --build --preset release && ctest --preset release")
