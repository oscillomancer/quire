#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fetches Tracktion's pluginval for this OS and prints the executable's path
# on stdout (everything else goes to stderr).
#   PLUGINVAL="$(get-pluginval.sh macos|linux|windows)"
set -euo pipefail
os="$1"
dest="$RUNNER_TEMP/pluginval"
rm -rf "$dest" && mkdir -p "$dest"

case "$os" in
    macos)   asset="pluginval_macOS.zip" ;;
    linux)   asset="pluginval_Linux.zip" ;;
    windows) asset="pluginval_Windows.zip" ;;
    *) echo "unknown os $os" >&2; exit 2 ;;
esac

gh release download --repo Tracktion/pluginval --pattern "$asset" --dir "$dest" >&2
unzip -q -o "$dest/$asset" -d "$dest" >&2

case "$os" in
    macos)   exe=$(find "$dest" -path "*pluginval.app/Contents/MacOS/pluginval" -type f | head -1) ;;
    windows) exe=$(find "$dest" -name pluginval.exe -type f | head -1) ;;
    *)       exe=$(find "$dest" -name pluginval -type f | head -1) ;;
esac
[ -n "$exe" ] || { echo "pluginval not found in $asset" >&2; exit 1; }
chmod +x "$exe" || true
echo "$exe"
