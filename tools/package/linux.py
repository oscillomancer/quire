#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a relocatable tar.xz and reject undeclared ELF dependencies."""
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
from common import options, run, stage

# Libraries provided by the OS/graphics drivers. Never bundle JACK or glibc.
BASE = re.compile(r"^(linux-vdso\.so.*|ld-linux.*|lib(c|m|dl|pthread|rt|resolv|util)\.so\..*|"
                  r"lib(GL|GLX|GLdispatch|OpenGL|EGL|GLESv2|X11|Xext|Xcursor|Xrandr|Xfixes|Xi|Xrender|Xss|Xinerama|Xau|Xdmcp|xcb|xcb-[\w-]+|asound|z)\.so\..*)$")
BUNDLED = ("libstdc++.so.6", "libgcc_s.so.1")


def check_dependencies(tree, slug):
    private = tree / "lib" / slug
    env = dict(os.environ, LD_LIBRARY_PATH=str(private))
    checked = 0
    for path in tree.rglob("*"):
        if not path.is_file():
            continue
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        checked += 1
        output = subprocess.check_output(["ldd", str(path)], env=env, text=True)
        for line in output.splitlines():
            line = line.strip()
            if not line:
                continue
            soname = line.split()[0]
            if soname.startswith("/"):
                soname = Path(soname).name
            if "not found" in line:
                raise RuntimeError(f"{path}: unresolved {line}")
            if soname.startswith("libjack"):
                raise RuntimeError(f"{path}: JACK must be dlopened, never linked/bundled")
            if (private / soname).exists():
                continue
            if not BASE.fullmatch(soname):
                raise RuntimeError(f"{path}: unbundled dependency {line}")
    print(f"Dependency audit passed: {checked} ELF files")


def runtime_notices(dest):
    # Ubuntu's copyright file includes GCC's licenses and runtime exception;
    # Arch separates the exception and common GPL text. Preserve the real texts.
    candidates = [Path("/usr/share/doc/libstdc++6/copyright"),
                  Path("/usr/share/doc/gcc-12-base/copyright"),
                  Path("/usr/share/doc/gcc-11-base/copyright")]
    notice = next((p for p in candidates if p.exists()), None)
    dest.mkdir(parents=True, exist_ok=True)
    if notice:
        shutil.copy2(notice, dest / "GCC-copyright.txt")
        gpl = Path("/usr/share/common-licenses/GPL-3")
    else:
        exception = Path("/usr/share/licenses/gcc/RUNTIME.LIBRARY.EXCEPTION")
        if not exception.exists():
            raise RuntimeError("GCC runtime exception notice not found; use the Ubuntu 22.04 packaging environment")
        shutil.copy2(exception, dest / exception.name)
        gpl = Path("/usr/share/licenses/spdx/GPL-3.0-only.txt")
    shutil.copy2(gpl, dest / "GPL-3.txt")


def main():
    args, product = options("linux")
    slug = product["slug"]
    arch = platform.machine()
    if arch != "x86_64":
        raise RuntimeError("Linux packages currently target x86_64")
    if 'VERSION_ID="22.04"' not in Path("/etc/os-release").read_text():
        print("Warning: release artifacts should be built on Ubuntu 22.04 for glibc compatibility")
    name = f'{slug}-{product["version"]}-linux-{arch}'
    with tempfile.TemporaryDirectory(prefix="package-", dir="/tmp") as temp:
        tree = stage(args.build_dir, Path(temp) / name)
        private = tree / "lib" / slug
        # The template's Linux plugins use shared libstdc++. Bundle one private
        # runtime for the DAW and its stock plugins, retaining exception notices.
        for soname in BUNDLED:
            runtime = subprocess.check_output(["c++", f"-print-file-name={soname}"], text=True).strip()
            if runtime == soname:
                raise RuntimeError(f"Cannot locate {soname}")
            shutil.copy2(Path(runtime).resolve(), private / soname)
        runtime_notices(tree / "share/doc" / slug / "third-party/GCC-runtime")
        check_dependencies(tree, slug)
        with tarfile.open(args.output_dir / f"{name}.tar.xz", "w:xz", preset=3) as archive:
            archive.add(tree, arcname=name)
    print(args.output_dir / f"{name}.tar.xz")


if __name__ == "__main__":
    main()
