#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-user NSIS installer and portable zip; no signing account required."""
from pathlib import Path
import shutil
import tempfile
from common import ROOT, archive_zip, options, run, stage



def nsis_path(path):
    return str(path).replace("/", "\\").replace("$", "$$").replace('"', '$\\"')


def write_manifests(tree, destination):
    """Generate exact deletion and lock-check lists from the staged payload."""
    def manifest(base, name):
        files = sorted(p.relative_to(base) for p in base.rglob("*") if p.is_file())
        dirs = sorted((p.relative_to(base) for p in base.rglob("*") if p.is_dir()),
                      key=lambda p: (-len(p.parts), str(p)))
        delete = []
        check = [f'!macro Check{name}Unlocked root failure unique']
        for index, relative in enumerate(files):
            path = nsis_path(relative)
            delete.extend([f'Delete "${{root}}\\{path}"',
                           f'IfFileExists "${{root}}\\{path}" ${{failure}}'])
            if relative.suffix.lower() in {".exe", ".dll", ".clap"}:
                check.extend([
                    f'IfFileExists "${{root}}\\{path}" 0 unlocked_${{unique}}_{index}',
                    f"System::Call 'kernel32::CreateFileW(w \"${{root}}\\{path}\", i 0x40000000, i 0, p 0, i 3, i 0, p 0) p .r1'",
                    'IntCmp $1 -1 ${failure}',
                    "System::Call 'kernel32::CloseHandle(p r1)'",
                    f'unlocked_${{unique}}_{index}:'])
        delete.extend(f'RMDir "${{root}}\\{nsis_path(directory)}"' for directory in dirs)
        delete.append('RMDir "${root}"')
        check.append('!macroend')
        text = '\n'.join([*check, f'!macro Delete{name}Files root failure', *delete, '!macroend', ''])
        (destination / f'{name.lower()}-files.nsh').write_text(text)
    manifest(tree, 'App')
    manifest(tree / 'plugins/clap', 'Plugin')

def main():
    args, product = options("windows")
    name = f'{product["slug"]}-{product["version"]}-windows-x86_64'
    nsis = shutil.which("makensis") or r"C:\Program Files (x86)\NSIS\makensis.exe"
    with tempfile.TemporaryDirectory(prefix="package-") as temp:
        tree = stage(args.build_dir, Path(temp) / "tree")
        archive_zip(tree, args.output_dir / f"{name}.zip")
        defines = {f"PRODUCT_{key.upper()}": value for key, value in product.items()}
        write_manifests(tree, Path(temp))
        defines.update(MANIFEST_DIR=str(Path(temp)), STAGE=str(tree), OUTPUT=str(args.output_dir / f"{name}.exe"))
        icon = ROOT / "assets/icon/app.ico"
        if icon.exists():
            defines["APP_ICON"] = str(icon)
        run(nsis, "/V3", *(f"/D{key}={value}" for key, value in defines.items()),
            ROOT / "tools/package/windows.nsi")
    print(args.output_dir / f"{name}.exe")


if __name__ == "__main__":
    main()
