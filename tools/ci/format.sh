#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# clang-format over first-party C/C++ sources.
# Usage: tools/ci/format.sh [--check]
set -euo pipefail
cd "$(dirname "$0")/../.."
mode=(-i)
[ "${1:-}" = "--check" ] && mode=(--dry-run -Werror)
mapfile -t files < <(git ls-files --cached --others --exclude-standard -- \
  'libs/*' 'host/*' 'plugins/*' 'daw/*' 'mcp/*' 'tools/*' 'tests/*' \
  | grep -E '\.(c|cc|cpp|h|hpp)$' | grep -v '/third_party/' \
  | grep -Ev '^plugins/(analyzer|delay|orrery|probe-synth)/test/vst3_unload_probe\.cpp$' \
  | grep -v '^plugins/probe-synth/' \
  | grep -v '^plugins/template/' \
  | grep -Ev '^plugins/delay/(plugin/gui/(Gui|Widgets|Window|Telemetry|pugl_metal)\.h|plugin/gui/(Window\.cpp|NanoVGBackend\.c)|plugin/src/delay_entry\.(h|cpp)|test/(host_(smoke\.cpp|contract\.h)|vst3_unload_probe\.cpp)|tools/((clap|linux)_host_probe\.cpp|wav\.h))$' \
  | grep -Ev '^plugins/analyzer/(plugin/gui/(Gui|Widgets|Window|Telemetry|pugl_metal)\.h|plugin/gui/(Window\.cpp|NanoVGBackend\.c)|plugin/src/analyzer_entry\.(h|cpp)|test/host_(smoke\.cpp|contract\.h)|tools/((clap|linux)_host_probe\.cpp|wav\.h))$' \
  | grep -Ev '^plugins/stock/[^/]+/Catalog\.cpp$' \
  | grep -v '^plugins/reverb/' \
  | grep -Ev '^plugins/orrery/(plugin/gui/(Gui|Widgets|Window|pugl_metal)\.h|plugin/gui/(Window\.cpp|NanoVGBackend\.c)|plugin/src/orrery_entry\.(h|cpp)|test/host_(smoke\.cpp|contract\.h)|tools/(clap|linux)_host_probe\.cpp)$' \
  || true)
# Template infrastructure stays byte-identical to the source copy apart from
# product naming. Check only product-owned code inside the copied plugin.
files+=(
  plugins/reverb/engine/include/reverb/{params,processor}.h
  plugins/reverb/engine/src/{params,processor}.cpp
  plugins/reverb/engine/tests/{measurements.h,test_reverb.cpp}
  plugins/reverb/engine/bench/reverb_bench.cpp
  plugins/reverb/plugin/src/{ReverbParams.h,reverb_clap.cpp}
  plugins/reverb/plugin/gui/{Editor.h,Editor.cpp,Theme.h}
  plugins/reverb/test/{test_core,editor_input,semantic_rt,tail_rt}.cpp
  plugins/reverb/tools/{reverb_cli,gui_preview}.cpp
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
