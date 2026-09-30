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
  | grep -v '^plugins/utility/' \
  | grep -v '^plugins/drive/' \
  | grep -Ev '^plugins/orrery/(plugin/gui/(Gui|Widgets|Window|pugl_metal)\.h|plugin/gui/(Window\.cpp|NanoVGBackend\.c)|plugin/src/orrery_entry\.(h|cpp)|test/host_(smoke\.cpp|contract\.h)|tools/(clap|linux)_host_probe\.cpp)$' \
  || true)
# Template infrastructure stays byte-identical to the source copy apart from
# product naming. Check only product-owned code inside the copied plugin.
files+=(
  plugins/drive/engine/include/drive/params.h
  plugins/drive/engine/include/drive/processor.h
  plugins/drive/engine/src/params.cpp
  plugins/drive/engine/src/processor.cpp
  plugins/drive/engine/tests/test_drive.cpp
  plugins/drive/engine/bench/drive_bench.cpp
  plugins/drive/plugin/src/DriveParams.h
  plugins/drive/plugin/src/drive_clap.cpp
  plugins/drive/plugin/gui/Editor.cpp
  plugins/drive/plugin/gui/Editor.h
  plugins/drive/plugin/gui/Theme.h
  plugins/drive/test/host_smoke.cpp
  plugins/drive/test/semantic_rt.cpp
  plugins/drive/test/editor_input.cpp
  plugins/drive/tools/gui_preview.cpp

  plugins/utility/engine/include/utility/params.h
  plugins/utility/engine/include/utility/processor.h
  plugins/utility/engine/src/params.cpp
  plugins/utility/engine/src/processor.cpp
  plugins/utility/engine/tests/test_utility.cpp
  plugins/utility/plugin/src/UtilityParams.h
  plugins/utility/plugin/src/utility_clap.cpp
  plugins/utility/plugin/gui/Editor.cpp
  plugins/utility/plugin/gui/Editor.h
  plugins/utility/plugin/gui/Theme.h
  plugins/utility/test/semantic_rt.cpp
  plugins/utility/test/editor_input.cpp
  plugins/utility/tools/gui_preview.cpp
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
