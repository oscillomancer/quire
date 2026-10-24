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
    """Persist ownership and generate preflight/removal lists for the payload."""
    def inventory(base):
        files = sorted(p.relative_to(base) for p in base.rglob("*") if p.is_file())
        dirs = sorted((p.relative_to(base) for p in base.rglob("*") if p.is_dir()),
                      key=lambda p: (-len(p.parts), str(p)))
        return files, dirs

    # Keep optional ownership in the app, so an unselected plugin directory
    # never acquires an inventory merely because the private CLAPs are shipped.
    plugin_files, plugin_dirs = inventory(tree / 'plugins/clap')
    def write_inventory(path, files, dirs):
        records = [f'F|{p.as_posix().replace("/", chr(92))}' for p in files]
        records += [f'D|{p.as_posix().replace("/", chr(92))}' for p in dirs]
        path.write_bytes(('\r\n'.join(records) + '\r\n').encode('utf-16-le'))
    write_inventory(tree / '.installed-plugins', plugin_files, plugin_dirs)
    (tree / '.installed-files').touch()
    app_files, app_dirs = inventory(tree)
    write_inventory(tree / '.installed-files', [*app_files, Path('uninstall.exe')], app_dirs)

    def manifest(base, name):
        files, dirs = inventory(base)
        # Ownership records survive failed removals and are deleted last.
        files.sort(key=lambda p: (p.name in {'.installed-files', '.installed-plugins'}, str(p)))
        delete = []
        safe = [f'!macro Check{name}Directories root failure prefix',
                'StrCpy $SafetyRoot "${root}"']
        preflight = [f'!macro Preflight{name} root inventory failure',
                     'StrCpy $InventoryFile "${inventory}"']
        for relative in [Path('.'), *dirs]:
            suffix = '' if relative == Path('.') else '\\' + nsis_path(relative)
            safe.extend([f'StrCpy $SafetyPath "${{root}}{suffix}"',
                         'Call ${prefix}CheckDirectoryChain',
                         'StrCmp $SafetyOK 1 0 ${failure}'])
        preflight_files = [*files, *([Path('uninstall.exe')] if name == 'App' else [])]
        for relative, kind in [*((p, 'D') for p in dirs), *((p, 'F') for p in preflight_files)]:
            path = nsis_path(relative)
            preflight.extend([
                f'StrCpy $CandidatePath "${{root}}\\{path}"',
                f'StrCpy $InventoryEntry "{kind}|{path}$\\r$\\n"',
                f'StrCpy $CandidateKind "{kind}"',
                'Call CheckDestination',
                'StrCmp $DestinationOK 1 0 ${failure}'])
        safe.append('!macroend')
        preflight.append('!macroend')
        check = [f'!macro Check{name}Unlocked root failure unique']
        for index, relative in enumerate(files):
            path = nsis_path(relative)
            parent = '' if relative.parent == Path('.') else '\\' + nsis_path(relative.parent)
            delete.extend(['StrCpy $SafetyRoot "${root}"',
                           f'StrCpy $SafetyPath "${{root}}{parent}"',
                           'Call un.CheckDirectoryChain',
                           'StrCmp $SafetyOK 1 0 ${failure}',
                           f'Delete "${{root}}\\{path}"',
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
        text = '\n'.join([*safe, *preflight, *check, f'!macro Delete{name}Files root failure', *delete, '!macroend', ''])
        (destination / f'{name.lower()}-files.nsh').write_text(text)
    manifest(tree, 'App')
    manifest(tree / 'plugins/clap', 'Plugin')

def build_installer(tree, destination, product, output):
    nsis = shutil.which("makensis") or r"C:\Program Files (x86)\NSIS\makensis.exe"
    defines = {f"PRODUCT_{key.upper()}": value for key, value in product.items()}
    write_manifests(tree, destination)
    defines.update(MANIFEST_DIR=str(destination), STAGE=str(tree), OUTPUT=str(output))
    icon = ROOT / "assets/icon/app.ico"
    if icon.exists():
        defines["APP_ICON"] = str(icon)
    run(nsis, "/V3", *(f"/D{key}={value}" for key, value in defines.items()),
        ROOT / "tools/package/windows.nsi")


def main():
    args, product = options("windows")
    name = f'{product["slug"]}-{product["version"]}-windows-x86_64'
    with tempfile.TemporaryDirectory(prefix="package-") as temp:
        tree = stage(args.build_dir, Path(temp) / "tree")
        archive_zip(tree, args.output_dir / f"{name}.zip")
        build_installer(tree, Path(temp), product, args.output_dir / f"{name}.exe")
    print(args.output_dir / f"{name}.exe")


if __name__ == "__main__":
    main()
