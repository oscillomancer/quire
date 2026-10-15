#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fetches free-audio/clap-validator's latest release for this OS and exports
# CLAP_VALIDATOR (into $GITHUB_ENV) for the configure step, which registers
# the clap-validator test with it.
#   get-clap-validator.sh macos|linux|windows
set -euo pipefail
os="$1"
dest="$RUNNER_TEMP/clap-validator"
rm -rf "$dest" && mkdir -p "$dest"

case "$os" in
    macos)   patterns=("*mac*" "*darwin*") ;;
    linux)   patterns=("*ubuntu*" "*linux*") ;;
    windows) patterns=("*windows*" "*win*") ;;
    *) echo "unknown os $os" >&2; exit 2 ;;
esac

echo "release assets:"
gh release view --repo free-audio/clap-validator --json tagName,assets \
    --jq '.tagName, (.assets[].name)' || true

for p in "${patterns[@]}"; do
    if gh release download --repo free-audio/clap-validator --pattern "$p" --dir "$dest" 2>/dev/null; then
        break
    fi
done

shopt -s nullglob
for archive in "$dest"/*; do
    case "$archive" in
        *.tar.gz|*.tgz) tar -xzf "$archive" -C "$dest" ;;
        *.zip) unzip -q -o "$archive" -d "$dest" ;;
    esac
done

exe=$(find "$dest" -type f \( -name clap-validator -o -name clap-validator.exe \) | head -1)
if [ -z "$exe" ]; then
    echo "no prebuilt clap-validator for $os; building it" >&2
    cargo install --locked --root "$dest" --git https://github.com/free-audio/clap-validator
    exe=$(find "$dest/bin" -type f \( -name clap-validator -o -name clap-validator.exe \) | head -1)
fi
chmod +x "$exe" || true
"$exe" --version || true
echo "CLAP_VALIDATOR=$exe" >> "$GITHUB_ENV"
