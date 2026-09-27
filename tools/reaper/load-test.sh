#!/usr/bin/env bash
# Phase 3 acceptance: load a stock instrument in REAPER on this machine.
# Usage: tools/reaper/load-test.sh <plugin display name> [out dir]
# The plugin must be in a CLAP path REAPER scans (e.g. ~/.clap). Starts REAPER
# with load_test.lua (new project, one track with the plugin, a MIDI phrase,
# a 5 s render, the editor opened), waits for the result, checks that the
# render has signal, and closes that REAPER instance without saving.
set -euo pipefail
fx="${1:?plugin display name, e.g. Orrery}"
out="${2:-/tmp/quire-reaper}"
mkdir -p "$out"
rm -f "$out/result.txt" "$out/render.wav"
if pgrep -x reaper >/dev/null; then echo "REAPER is already running; close it first" >&2; exit 2; fi
QUIRE_REAPER_FX="$fx" QUIRE_REAPER_OUT="$out" reaper -nosplash "$(dirname "$0")/load_test.lua" \
  >"$out/reaper.log" 2>&1 &
pid=$!
for _ in $(seq 1 90); do
  grep -qs "editor_open\|FAIL" "$out/result.txt" && break
  sleep 1
done
sleep "${QUIRE_REAPER_LINGER:-3}"  # time for a screenshot of the editor
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
cat "$out/result.txt" 2>/dev/null || { echo "no result from REAPER" >&2; exit 1; }
grep -q "FAIL" "$out/result.txt" && exit 1
python3 - "$out/render.wav" <<'PY'
import math, sys, wave
w = wave.open(sys.argv[1]); sw = w.getsampwidth(); data = w.readframes(w.getnframes())
peak = max(abs(int.from_bytes(data[i:i + sw], "little", signed=True)) for i in range(0, len(data), sw))
peak /= float(1 << (8 * sw - 1))
print("render peak %.1f dBFS" % (20 * math.log10(peak) if peak > 0 else -math.inf))
sys.exit(0 if peak > 1e-3 else 1)
PY
