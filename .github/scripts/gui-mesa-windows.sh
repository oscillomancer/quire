#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# CI-only software GL. Never installed into the product or the workstation.
set -euo pipefail
root="$GITHUB_WORKSPACE/.deps/gui-mesa-24.3.4"
archive="$root/mesa.7z"
mkdir -p "$root"
if [[ ! -f "$archive" ]]; then
  curl -fL --retry 3 https://github.com/pal1000/mesa-dist-win/releases/download/24.3.4/mesa3d-24.3.4-release-msvc.7z -o "$archive"
fi
printf '%s  %s\n' 7ebc711ad1896ac88ab21e142f1017f8ff035f0f342bdb72fbb5e2eb881ba363 "$archive" | sha256sum -c -
7z x -y "$archive" "-o$root" 'x64/*' > /dev/null
# DLL search begins beside each test/app executable. Deploy just the GL driver
# and its shared dispatch library; no machine-wide registration or installers.
while IFS= read -r -d '' executable; do
  destination=$(dirname "$executable")
  for dll in opengl32.dll libglapi.dll libgallium_wgl.dll; do
    cp "$root/x64/$dll" "$destination/$dll"
  done
done < <(find build/windows/daw/gui -maxdepth 1 -name '*.exe' -print0)
echo 'GALLIUM_DRIVER=llvmpipe' >> "$GITHUB_ENV"
echo 'LIBGL_ALWAYS_SOFTWARE=1' >> "$GITHUB_ENV"
