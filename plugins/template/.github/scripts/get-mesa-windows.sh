#!/usr/bin/env bash
# Puts Mesa's software OpenGL (llvmpipe, from pal1000/mesa-dist-win) beside
# the given executables. GitHub's Windows runners have no GPU driver, so
# their opengl32.dll is Windows' GDI renderer, which stops at OpenGL 1.1 and
# cannot run the editor's GL 2 shaders. Windows loads a DLL from the
# executable's own directory before System32, so a plugin loaded by an
# executable there renders through Mesa instead.
#   get-mesa-windows.sh <directory>...
set -euo pipefail
dest="$RUNNER_TEMP/mesa-24.3.4"
if [ ! -d "$dest/x64" ]; then
    rm -rf "$dest" && mkdir -p "$dest"
    curl -fL --retry 3 https://github.com/pal1000/mesa-dist-win/releases/download/24.3.4/mesa3d-24.3.4-release-msvc.7z -o "$dest/mesa.7z"
    printf '%s  %s\n' 7ebc711ad1896ac88ab21e142f1017f8ff035f0f342bdb72fbb5e2eb881ba363 "$dest/mesa.7z" | sha256sum -c -
    (cd "$dest" && 7z x -y -bso0 mesa.7z)
fi
for dir in "$@"; do
    mkdir -p "$dir"
    # Only the WGL dependencies. Copying the D3D replacement DLLs as well
    # intercepts pluginval/JUCE's Direct2D renderer (24.3.4 aborts on RET).
    for dll in opengl32.dll libglapi.dll libgallium_wgl.dll; do
        cp "$dest/x64/$dll" "$dir/"
    done
    echo "Mesa llvmpipe OpenGL placed in $dir"
done
echo "GALLIUM_DRIVER=llvmpipe" >> "$GITHUB_ENV"
