# Compares the sandbox's generator output with the files committed under
# skill/generated/. Regenerate with tools/gen/update-generated.sh.
foreach(pair "--markdown;sandbox-commands.md" "--mcp-tools;sandbox-mcp-tools.json")
  list(GET pair 0 flag)
  list(GET pair 1 file)
  execute_process(COMMAND ${TOOL} registry ${flag} OUTPUT_VARIABLE out RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${TOOL} registry ${flag} failed (${rc})")
  endif()
  file(READ ${GENERATED}/${file} committed)
  if(NOT out STREQUAL committed)
    message(FATAL_ERROR "skill/generated/${file} is out of date: run tools/gen/update-generated.sh")
  endif()
endforeach()
