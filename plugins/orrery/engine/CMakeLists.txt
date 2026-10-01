# Orrery's SDK-free core (docs/specs/synth.md): parameter table, patch codec
# and the real-time synth engine. Built and tested by the monorepo with its
# warnings and sanitizers; the CLAP plugin (plugins/orrery, template-derived)
# links the same sources.
supple_add_library(orrery-engine
  SOURCES src/params.cpp src/patch.cpp src/engine.cpp src/fx.cpp src/oversampling.cpp
          src/wavetables.cpp
  DEPS supple::core-base supple::core-json supple::core-params supple::core-patch
       supple::core-dsp)

supple_add_tests(orrery-engine
  SOURCES tests/test_params.cpp tests/test_patch.cpp tests/test_engine.cpp tests/test_wavetables.cpp tests/test_guide.cpp
  LABELS rt)

if(TARGET orrery-engine.tests)
  target_compile_definitions(orrery-engine.tests PRIVATE SUPPLE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

# CPU benchmark (synth.md §9); meaningful in the release preset. A monorepo
# developer tool: not built inside template-derived plugin projects.
if(SUPPLE_BUILD_TESTS)
  add_executable(orrery-bench bench/orrery_bench.cpp)
  target_link_libraries(orrery-bench PRIVATE supple::orrery-engine)
  supple_target_defaults(orrery-bench)

  # Optional DSP snapshot from git archive for repeatable numerical/CPU
  # comparisons. Use the current capture harness with the unchanged old core.
  if(SUPPLE_ORRERY_BASELINE_DIR)
    add_executable(orrery-reference-bench bench/orrery_bench.cpp)
    foreach(source params patch engine fx oversampling wavetables)
      target_sources(orrery-reference-bench PRIVATE
        "${SUPPLE_ORRERY_BASELINE_DIR}/src/${source}.cpp")
    endforeach()
    target_include_directories(orrery-reference-bench PRIVATE
      "${SUPPLE_ORRERY_BASELINE_DIR}/include" "${SUPPLE_ORRERY_BASELINE_DIR}/src")
    target_link_libraries(orrery-reference-bench PRIVATE
      supple::core-base supple::core-json supple::core-params supple::core-patch supple::core-dsp)
    supple_target_defaults(orrery-reference-bench)
  endif()
endif()
