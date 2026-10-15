#!/usr/bin/env bash
# The CI wrapper must reject the cached-size false positive, native/report
# mismatches independently, and a silent plugin; a note effect's notes count. Runs on Linux too, without Cocoa or a GPU.
set -euo pipefail
checker="$1"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/build/release"
cat > "$work/build/release/clap-probe" <<'PROBE'
#!/usr/bin/env bash
size=900x560
for arg in "$@"; do
    [ "$arg" != --presize ] || size=1125x700
done
echo 'presize: adjust->1125x700 set_size=1'
echo "gui: create=1 parent=1 show=1 size=$size"
echo 'reopen: create=1 parent=1 show=1'
echo 'resize: adjust->1400x896 set_size=1'
case "$PROBE_CASE" in
    notes) echo 'notes out         : 3  (note path is live)' ;;
    silent) echo 'output peak       : -120.000 dBFS  (silent: unexpected)' ;;
    *) echo 'output peak       : -14.0 dBFS  (audio path is live)' ;;
esac
case "$PROBE_CASE" in
    cached) ;; # Old probe output: the size was read before parenting.
    native) echo "realized: get_size=1 size=$size native=800.0x500.0 expected=$size matches=0" ;;
    reported) echo "realized: get_size=1 size=800x500 native=$size expected=$size matches=0" ;;
    wrong-expected) echo "realized: get_size=1 size=800x500 native=800.0x500.0 expected=800x500 matches=1" ;;
    *) echo "realized: get_size=1 size=$size native=$size expected=$size matches=1" ;;
esac
if [ "$PROBE_CASE" = resize ]; then
    echo 'resized: get_size=1 size=900x560 native=900.0x560.0 expected=1400x896 matches=0'
else
    echo 'resized: get_size=1 size=1400x896 native=1400.0x896.0 expected=1400x896 matches=1'
fi
PROBE
chmod +x "$work/build/release/clap-probe"
cd "$work"
for mode in cached native reported wrong-expected resize silent; do
    if PROBE_CASE="$mode" bash "$checker" Starter > "$mode.log" 2>&1; then
        echo "checker accepted $mode sizing failure" >&2
        exit 1
    fi
done
PROBE_CASE=valid bash "$checker" Starter > valid.log 2>&1
PROBE_CASE=notes bash "$checker" Starter > notes.log 2>&1
echo 'CI probe checker rejects cached, native, reported and resize failures and silence, and takes a note effect'
