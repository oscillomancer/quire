#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-user NSIS installer and portable zip; no signing account required."""
from pathlib import Path
import shutil
import tempfile
from common import ROOT, archive_zip, options, run, stage


def main():
    args, product = options("windows")
    name = f'{product["slug"]}-{product["version"]}-windows-x86_64'
    nsis = shutil.which("makensis") or r"C:\Program Files (x86)\NSIS\makensis.exe"
    with tempfile.TemporaryDirectory(prefix="package-") as temp:
        tree = stage(args.build_dir, Path(temp) / "tree")
        archive_zip(tree, args.output_dir / f"{name}.zip")
        defines = {f"PRODUCT_{key.upper()}": value for key, value in product.items()}
        defines.update(STAGE=str(tree), OUTPUT=str(args.output_dir / f"{name}.exe"))
        icon = ROOT / "assets/icon/app.ico"
        if icon.exists():
            defines["APP_ICON"] = str(icon)
        run(nsis, "/V3", *(f"/D{key}={value}" for key, value in defines.items()),
            ROOT / "tools/package/windows.nsi")
    print(args.output_dir / f"{name}.exe")


if __name__ == "__main__":
    main()
