# SPDX-License-Identifier: GPL-3.0-or-later
# Executed at install time after the five programs have been built.
if(NOT CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM)
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM windows+pe)
endif()
if(NOT CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL)
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL dumpbin)
  find_program(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND NAMES dumpbin REQUIRED)
endif()
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES ${runtime_executables}
  MODULES ${runtime_modules}
  DIRECTORIES "${runtime_search}"
  RESOLVED_DEPENDENCIES_VAR resolved
  UNRESOLVED_DEPENDENCIES_VAR unresolved
  CONFLICTING_DEPENDENCIES_PREFIX conflicts
  # API sets and OpenGL are OS facilities even if CI put Mesa beside the GUI.
  PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*" "opengl32\\.dll"
  POST_INCLUDE_REGEXES ".*[Mm][Ss][Vv][Cc][Pp]140.*\\.[Dd][Ll][Ll]$"
                       ".*[Vv][Cc][Rr][Uu][Nn][Tt][Ii][Mm][Ee]140.*\\.[Dd][Ll][Ll]$"
                       ".*[Cc][Oo][Nn][Cc][Rr][Tt]140\\.[Dd][Ll][Ll]$"
  POST_EXCLUDE_REGEXES ".*/[Ss][Yy][Ss][Tt][Ee][Mm]32/.*"
                       ".*/[Ss][Yy][Ss][Ww][Oo][Ww]64/.*")
if(unresolved OR conflicts_FILENAMES)
  message(FATAL_ERROR "Runtime dependencies unresolved: ${unresolved}; conflicting: ${conflicts_FILENAMES}")
endif()
set(names)
foreach(library IN LISTS resolved)
  get_filename_component(name "${library}" NAME)
  string(TOLOWER "${name}" lower_name)
  if(lower_name MATCHES "^(msvcp140.*|vcruntime140.*|concrt140)\\.dll$")
    # Use the compiler's redistribution directory, never a workstation's CRT.
    set(redist_source "")
    foreach(candidate IN LISTS compiler_redist)
      get_filename_component(candidate_name "${candidate}" NAME)
      string(TOLOWER "${candidate_name}" candidate_name)
      if(candidate_name STREQUAL lower_name)
        set(redist_source "${candidate}")
        break()
      endif()
    endforeach()
    if(NOT redist_source)
      message(FATAL_ERROR "Missing compiler redistributable for ${name}")
    endif()
    set(library "${redist_source}")
  endif()
  file(INSTALL DESTINATION "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}" TYPE FILE FILES "${library}")
  list(APPEND names "${lower_name}")
endforeach()
list(REMOVE_DUPLICATES names)
list(SORT names)
string(REPLACE ";" "\n" inventory "${names}")
file(WRITE "${runtime_inventory}" "${inventory}\n")
