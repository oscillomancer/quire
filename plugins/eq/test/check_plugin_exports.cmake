# The built Linux modules must export their entry points and nothing else.
# Anything more is visible to every other plugin in the host process, where
# the GNU unique symbols among it (fmt's locale facet id, the VST3 SDK's
# UpdateHandler singleton) bind across modules whatever RTLD_LOCAL says. Run
# with -DMODULE=<path> and -DEXPECTED=<a,b,c>.
find_program(NM_TOOL nm)
if (NOT NM_TOOL)
    message(STATUS "nm not found; skipping the export check")
    return()
endif()
if (NOT EXISTS "${MODULE}")
    message(FATAL_ERROR "module not built: ${MODULE}")
endif()

execute_process(COMMAND ${NM_TOOL} -D --defined-only "${MODULE}"
    OUTPUT_VARIABLE listing RESULT_VARIABLE status)
if (NOT status EQUAL 0)
    message(FATAL_ERROR "nm failed on ${MODULE}")
endif()

string(REGEX REPLACE "\n$" "" listing "${listing}")
string(REPLACE "\n" ";" lines "${listing}")
set(exported "")
foreach(line ${lines})
    string(REGEX REPLACE "^[0-9a-fA-F]+ [A-Za-z] " "" name "${line}")
    list(APPEND exported "${name}")
endforeach()

list(SORT exported)
string(REPLACE "," ";" expected "${EXPECTED}")
list(SORT expected)
if (NOT exported STREQUAL expected)
    list(LENGTH exported count)
    message(FATAL_ERROR
        "${MODULE} exports ${count} symbols, expected exactly: ${expected}\n"
        "found: ${exported}")
endif()
