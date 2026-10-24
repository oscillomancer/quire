#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fix private dylib references, sign inside-out, and produce a dmg and zip."""
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
from common import archive_zip, options, run, stage


def is_macho(path):
    return path.is_file() and "Mach-O" in subprocess.check_output(["file", "-b", str(path)], text=True)


def finish_bundle(app):
    frameworks = app / "Contents/Frameworks"
    binaries = [p for p in app.rglob("*") if is_macho(p) and not p.is_symlink()]
    dylibs = {p.name: p for p in frameworks.glob("*.dylib")}
    for binary in binaries:
        deps = subprocess.check_output(["otool", "-L", str(binary)], text=True).splitlines()[1:]
        for line in deps:
            old = line.strip().split(" (", 1)[0]
            if old.startswith(("/System/Library/", "/usr/lib/", "@")):
                continue
            if Path(old).name not in dylibs:
                raise RuntimeError(f"Unbundled dependency: {binary}: {old}")
            run("install_name_tool", "-change", old, f"@rpath/{Path(old).name}", binary)
        if binary.parent == frameworks:
            run("install_name_tool", "-id", f"@rpath/{binary.name}", binary)
    for binary in sorted(binaries, key=lambda p: len(p.parts), reverse=True):
        run("codesign", "--force", "--sign", "-", binary)
    for plugin in app.glob("Contents/PlugIns/CLAP/*.clap"):
        run("codesign", "--force", "--sign", "-", plugin)
    run("codesign", "--force", "--sign", "-", app)
    run("codesign", "--verify", "--deep", "--strict", app)


def main():
    args, product = options("macos")
    if platform.machine() != "arm64":
        raise RuntimeError("The macOS package supports Apple Silicon only")
    name = f'{product["slug"]}-{product["version"]}-macos-arm64'
    with tempfile.TemporaryDirectory(prefix="package-") as temp:
        tree = stage(args.build_dir, Path(temp) / "tree")
        app = tree / f'{product["name"]}.app'
        finish_bundle(app)
        plugins = tree / "Stock plugins for other hosts"
        shutil.copytree(app / "Contents/PlugIns/CLAP", plugins)
        (plugins / "README.txt").write_text(
            "Optional: copy the .clap bundles and accompanying notices to\n"
            "~/Library/Audio/Plug-Ins/CLAP (create it if necessary).\n"
            "The application already contains these plugins for its own use.\n")
        (tree / "Applications").symlink_to("/Applications")
        run("hdiutil", "create", "-volname", product["name"], "-srcfolder", tree,
            "-ov", "-format", "UDZO", args.output_dir / f"{name}.dmg")
        # ditto preserves bundle permissions, symlinks, and macOS metadata.
        run("ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", app,
            args.output_dir / f"{name}.zip")
    print(args.output_dir / f"{name}.dmg")


if __name__ == "__main__":
    main()
