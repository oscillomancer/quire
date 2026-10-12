#!/usr/bin/env bash
# Puts Mesa's software OpenGL (llvmpipe, from pal1000/mesa-dist-win) beside
# the given executables. GitHub's Windows runners have no GPU driver, so
# their opengl32.dll is Windows' GDI renderer, which stops at OpenGL 1.1 and
# cannot run the editor's GL 2 shaders. Windows loads a DLL from the
# executable's own directory before System32, so a plugin loaded by an
# executable there renders through Mesa instead.
#   get-mesa-windows.sh <directory>...
set -euo pipefail
dest="$RUNNER_TEMP/mesa"
if [ ! -d "$dest/x64" ]; then
    rm -rf "$dest" && mkdir -p "$dest"
    gh release download --repo pal1000/mesa-dist-win --pattern "*release-msvc.7z" --dir "$dest"
    (cd "$dest" && 7z x -y -bso0 ./*.7z)
fi
for dir in "$@"; do
    mkdir -p "$dir"
    cp "$dest"/x64/*.dll "$dir/"
    echo "Mesa llvmpipe OpenGL placed in $dir"
done
echo "GALLIUM_DRIVER=llvmpipe" >> "$GITHUB_ENV"
