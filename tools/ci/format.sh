#!/usr/bin/env bash
# clang-format over first-party C/C++ sources.
# Usage: tools/ci/format.sh [--check]
set -euo pipefail
cd "$(dirname "$0")/../.."
mode=(-i)
[ "${1:-}" = "--check" ] && mode=(--dry-run -Werror)
mapfile -t files < <(git ls-files --cached --others --exclude-standard -- \
  'libs/*' 'host/*' 'plugins/*' 'daw/*' 'mcp/*' 'tools/*' 'tests/*' \
  | grep -E '\.(c|cc|cpp|h|hpp)$' | grep -v '/third_party/' \
  | grep -v '^plugins/probe-synth/' \
  | grep -v '^plugins/eq/' \
  | grep -Ev '^plugins/orrery/(plugin/gui/(Gui|Widgets|Window|pugl_metal)\.h|plugin/gui/(Window\.cpp|NanoVGBackend\.c)|plugin/src/orrery_entry\.(h|cpp)|test/host_(smoke\.cpp|contract\.h)|tools/(clap|linux)_host_probe\.cpp)$' \
  || true)
# Template infrastructure stays byte-identical to the source copy apart from
# product naming. Check only product-owned code inside the copied plugin.
files+=(
  plugins/eq/engine/include/eq/params.h
  plugins/eq/engine/include/eq/processor.h
  plugins/eq/engine/src/{params,processor,linear}.cpp
  plugins/eq/engine/src/{design,linear,matched}.h
  plugins/eq/engine/tests/test_eq.cpp
  plugins/eq/engine/bench/{accuracy.h,eq_bench.cpp}
  plugins/eq/plugin/src/EqParams.h
  plugins/eq/plugin/src/eq_clap.cpp
  plugins/eq/plugin/gui/Editor.{h,cpp}
  plugins/eq/plugin/gui/Telemetry.h
  plugins/eq/test/{test_core,editor_input,semantic_rt}.cpp
  plugins/eq/tools/{gui_preview,clap_host_probe}.cpp
  plugins/probe-synth/dsp/include/probesynth/Processor.h
  plugins/probe-synth/plugin/src/ProbeSynthParams.h
  plugins/probe-synth/plugin/src/probesynth_clap.cpp
  plugins/probe-synth/plugin/gui/Editor.cpp
  plugins/probe-synth/plugin/gui/Editor.h
  plugins/probe-synth/test/test_core.cpp
  plugins/probe-synth/test/editor_input.cpp
  plugins/probe-synth/test/semantic_rt.cpp
  plugins/probe-synth/tools/probesynth_cli.cpp
  plugins/probe-synth/tools/gui_preview.cpp
)
[ ${#files[@]} -eq 0 ] && exit 0
clang-format "${mode[@]}" "${files[@]}"
