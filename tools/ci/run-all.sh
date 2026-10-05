#!/usr/bin/env bash
# Local CI: every preset, the license gate and the format check.
# Usage: tools/ci/run-all.sh [preset ...]   (default: all)
set -uo pipefail
cd "$(dirname "$0")/../.."
presets=("$@")
[ ${#presets[@]} -eq 0 ] && presets=(dev release gcc asan ubsan tsan rtsan)
fail=0
declare -A result
python3 tools/license-check/check.py --root . && result[license]=pass || { result[license]=FAIL; fail=1; }
tools/ci/format.sh --check >/dev/null && result[format]=pass || { result[format]=FAIL; fail=1; }
for p in "${presets[@]}"; do
  mkdir -p build; if cmake --workflow --preset "$p" > "build/ci-$p.log" 2>&1; then
    result[$p]=pass
  else
    result[$p]=FAIL; fail=1
  fi
done
echo "== local CI summary"
for k in license format "${presets[@]}"; do printf '  %-8s %s\n' "$k" "${result[$k]}"; done
[ $fail -eq 0 ] || echo "logs: build/ci-<preset>.log"
exit $fail
