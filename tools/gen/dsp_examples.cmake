# The skill's guide to the example scripts, made from them:
#
#   cmake -DEXAMPLES=plugins/suite/examples -DOUT=skill/generated/dsp-examples.md
#         [-DCHECK=ON] -P tools/gen/dsp_examples.cmake
#
# writes OUT, or with CHECK fails when OUT is not what it would write (the
# drift test): a table of the examples, then each one's code.

# In the order an agent should meet them; the tour of the language is a test,
# not an example.
set(order gain tilt compressor echo drive spectral reverb synth sampler)
set(skip language)

file(GLOB found RELATIVE "${EXAMPLES}" "${EXAMPLES}/*.cpp")
foreach(file IN LISTS found)
  string(REPLACE ".cpp" "" name "${file}")
  if(NOT name IN_LIST order AND NOT name IN_LIST skip)
    message(FATAL_ERROR "the example ${file} is not in tools/gen/dsp_examples.cmake's order (or skip)")
  endif()
endforeach()

set(text "# DSP scripts: the examples\n\n")
string(APPEND text "Generated from `plugins/suite/examples` by `tools/gen/update-generated.sh`.\n")
string(APPEND text "Every one is tested: compiled to machine code, interpreted and compiled as\n")
string(APPEND text "C++, with the same output, and played by the script player. Start from the\n")
string(APPEND text "closest, change it, and keep what works (`guides/dsp-language.md` is the\n")
string(APPEND text "language, `guides/dsp-workflow.md` the commands).\n\n")
string(APPEND text "| Example | What it is |\n|---|---|\n")
set(sections "")
foreach(name IN LISTS order)
  file(READ "${EXAMPLES}/${name}.cpp" body)
  # Its first comment, as one line of the table.
  string(REGEX MATCH "\n\n((//[^\n]*\n)+)" comment "${body}")
  set(about "${CMAKE_MATCH_1}")
  string(REGEX REPLACE "// ?" "" about "${about}")
  string(REGEX REPLACE "\n" " " about "${about}")
  string(STRIP "${about}" about)
  string(APPEND text "| [${name}](#${name}) | ${about} |\n")
  string(APPEND sections "\n## ${name}\n\n```cpp\n${body}```\n")
endforeach()
string(APPEND text "${sections}")

if(CHECK)
  file(READ "${OUT}" committed)
  if(NOT committed STREQUAL text)
    message(FATAL_ERROR "skill/generated/dsp-examples.md is out of date: run tools/gen/update-generated.sh")
  endif()
else()
  file(WRITE "${OUT}" "${text}")
endif()
