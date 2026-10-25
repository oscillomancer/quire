# The CLAP extension headers are kept twice: include/supple/clap/ here, which
# the host compiles against, and plugins/suite/semantic/include/supple/clap/,
# which plugins and source exports compile against. Each side compiles
# cleanly against its own copy, so a copy that drifts shows only as an
# extension the host quietly finds missing, or calls through the wrong
# member. This checks the two hold the same headers, byte for byte.
# Required: HOST (this folder's include/supple/clap), PLUGIN (the suite's).
foreach(var HOST PLUGIN)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "header_copies: -D${var}=... is required")
    endif()
endforeach()

file(GLOB host_headers RELATIVE "${HOST}" "${HOST}/*.h")
file(GLOB plugin_headers RELATIVE "${PLUGIN}" "${PLUGIN}/*.h")
list(SORT host_headers)
list(SORT plugin_headers)
if(NOT host_headers STREQUAL plugin_headers)
    message(FATAL_ERROR "the extension headers differ: ${HOST} has [${host_headers}], "
                        "${PLUGIN} has [${plugin_headers}]; keep a copy of each in both")
endif()
foreach(header IN LISTS host_headers)
    file(SHA256 "${HOST}/${header}" host_hash)
    file(SHA256 "${PLUGIN}/${header}" plugin_hash)
    if(NOT host_hash STREQUAL plugin_hash)
        message(FATAL_ERROR "${header} differs between ${HOST} and ${PLUGIN}: "
                            "make the change in both, byte for byte")
    endif()
endforeach()
