#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# SDL3 is a declared system dependency. Build it in the disposable Linux
# runner's workspace because Ubuntu 24.04 predates its development package.
set -euo pipefail
version=3.2.30
root="$GITHUB_WORKSPACE/.deps"
if [[ -f "$root/install/lib/cmake/SDL3/SDL3Config.cmake" ]]; then exit 0; fi
mkdir -p "$root"
curl -fL --retry 3 "https://github.com/libsdl-org/SDL/releases/download/release-$version/SDL3-$version.tar.gz" -o "$root/sdl3.tar.gz"
tar -xzf "$root/sdl3.tar.gz" -C "$root"
cmake -S "$root/SDL3-$version" -B "$root/sdl3-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$root/install" \
  -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_STATIC=OFF
cmake --build "$root/sdl3-build" --parallel 6
cmake --install "$root/sdl3-build"
