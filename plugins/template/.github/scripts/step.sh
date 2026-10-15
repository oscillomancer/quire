#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Runs one CI command with its output kept in logs/<name>.log, and on failure
# repeats the useful part of that output as error annotations: the compiler
# and linker diagnostics first, then the tail of the log. Annotations are
# readable from the checks API, so a failure can be diagnosed without
# downloading the full job log.
#   step.sh <name> <command> [args...]
set -uo pipefail
name="$1"
shift
mkdir -p logs
log="logs/$name.log"

"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
[ "$status" -eq 0 ] && exit 0

# Workflow commands need %, CR and LF escaped.
emit() {
    local title="$1" body="$2"
    body="${body//'%'/'%25'}"
    body="${body//$'\r'/'%0D'}"
    body="${body//$'\n'/'%0A'}"
    echo "::error title=$title::$body"
}

# Up to 6 annotations of diagnostics, 3500 characters each, then up to 3 of
# the log's tail. GitHub keeps at most 10 error annotations per step.
diagnostics=$(grep -n -E -A4 \
    "error[: ]|Error |FAILED|Undefined symbols|ld: |fatal|\*\*\*Failed|Assertion|CHECK failed|FAIL\b" \
    "$log" | grep -v "^--$" | head -c 21000)
n=0
while [ -n "$diagnostics" ] && [ $n -lt 6 ]; do
    emit "$name: diagnostics ($((n + 1)))" "${diagnostics:0:3500}"
    diagnostics="${diagnostics:3500}"
    n=$((n + 1))
done
tail=$(tail -c 10500 "$log")
n=0
while [ -n "$tail" ] && [ $n -lt 3 ]; do
    emit "$name: log tail ($((n + 1)))" "${tail:0:3500}"
    tail="${tail:3500}"
    n=$((n + 1))
done
exit "$status"
