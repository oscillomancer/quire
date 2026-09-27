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
  SOURCES tests/test_params.cpp tests/test_patch.cpp tests/test_engine.cpp tests/test_wavetables.cpp
  LABELS rt)

# CPU benchmark (synth.md §9); meaningful in the release preset. A monorepo
# developer tool: not built inside template-derived plugin projects.
if(SUPPLE_BUILD_TESTS)
  add_executable(orrery-bench bench/orrery_bench.cpp)
  target_link_libraries(orrery-bench PRIVATE supple::orrery-engine)
  supple_target_defaults(orrery-bench)
endif()
