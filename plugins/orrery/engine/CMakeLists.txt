# Orrery's SDK-free core (docs/specs/synth.md): parameter table, patch codec
# and the real-time synth engine. Built and tested by the monorepo with its
# warnings and sanitizers; the CLAP plugin (plugins/orrery, template-derived)
# links the same sources.
supple_add_library(orrery-engine
  SOURCES src/params.cpp src/patch.cpp src/engine.cpp src/fx.cpp src/oversampling.cpp
  DEPS supple::core-base supple::core-json supple::core-params supple::core-patch)

supple_add_tests(orrery-engine
  SOURCES tests/test_params.cpp tests/test_patch.cpp tests/test_engine.cpp
  LABELS rt)
