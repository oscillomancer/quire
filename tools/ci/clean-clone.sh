#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Phase 0 acceptance gate: a fresh clone builds and tests with one command,
# offline. Clones HEAD (committed state only) into a temporary directory,
# runs the dev workflow there, and removes it afterwards.
# Usage: tools/ci/clean-clone.sh [preset]   (default: dev)
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
preset="${1:-dev}"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/clean-clone.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
git clone -q --no-hardlinks "$repo" "$tmp/src"
cd "$tmp/src"
echo "clean-clone: $(git log --oneline -1) -> $tmp/src (preset $preset)"
# Any network access during configure/build is a failure of the vendoring policy.
# FetchContent reads this as a cache variable; an environment variable has no effect.
if { cmake --preset "$preset" -DFETCHCONTENT_FULLY_DISCONNECTED=ON &&
     cmake --build --preset "$preset" &&
     ctest --preset "$preset"; } > "$tmp/build.log" 2>&1; then
  grep -E 'tests passed|Total Test' "$tmp/build.log" | tail -2
  echo "clean-clone: PASS"
else
  tail -40 "$tmp/build.log"
  echo "clean-clone: FAIL"
  exit 1
fi
