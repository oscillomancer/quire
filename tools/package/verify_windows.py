#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native NSIS ownership regressions using two real staged payload versions."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import winreg
import zipfile
from windows import build_installer
from common import ROOT


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--syntax-only', action='store_true')
    args = parser.parse_args()
    if args.syntax_only:
        # Compile all real safety instructions before the expensive C++ build.
        with tempfile.TemporaryDirectory(prefix='nsis-syntax-') as temp:
            root = Path(temp)
            tree = root / 'stage'
            (tree / 'licenses').mkdir(parents=True)
            shutil.copyfile(ROOT / 'LICENSE', tree / 'licenses/LICENSE')
            (tree / 'plugins/clap').mkdir(parents=True)
            (tree / 'plugins/clap/Stock.clap').write_bytes(b'fixture')
            (tree / 'example.exe').write_bytes(b'fixture')
            build_installer(tree, root, dict(name='Example', slug='example', version='0.0.0',
                                            vendor='Example Vendor', vendor_id='org.example'), root / 'syntax.exe')
        print('NSIS safety instruction compilation passed')
        return
    if not args.build_dir or not args.archive:
        parser.error('--build-dir and --archive are required without --syntax-only')
    product = json.loads((args.build_dir / 'package/product.json').read_text())
    slug = product['slug']
    registry = rf'Software\Microsoft\Windows\CurrentVersion\Uninstall\{slug}'
    plugins = Path(os.environ['LOCALAPPDATA']) / 'Programs/Common/CLAP' / slug
    assert not plugins.exists(), 'Run after the default-component installer smoke has uninstalled'
    with tempfile.TemporaryDirectory(prefix='installer-regression-') as temp:
        root = Path(temp)
        old, new = root / 'old', root / 'new'
        with zipfile.ZipFile(args.archive) as archive:
            archive.extractall(old)
        (old / 'version-marker.txt').write_text('old')
        shutil.copytree(old, new)
        (new / 'version-marker.txt').write_text('new')
        (new / 'skill/guides/added-by-upgrade.md').write_text('new guide')
        (new / 'skill/new-folder').mkdir()
        (new / 'skill/new-folder/new.md').write_text('new nested guide')
        (new / 'plugins/clap/UpgradeExtra-NOTICES.txt').write_text('new plugin notice')
        for label, stage in (('old', old), ('new', new)):
            manifests = root / (label + '-manifests')
            manifests.mkdir()
            build_installer(stage, manifests, product, root / (label + '.exe'))
        installed = root / 'installed'

        def install(label, expected=0, *options):
            result = subprocess.run([str(root / (label + '.exe')), '/S', *options, f'/D={installed}'])
            assert (result.returncode == 0) == (expected == 0), f'{label} installer exit {result.returncode}'

        install('old', 0, '/PLUGINS')
        inventory = (installed / '.installed-files').read_bytes()
        assert len(list(plugins.glob('*.clap'))) == product['stock_plugins']
        (plugins / 'notes.txt').write_text('user plugin notes')
        (installed / 'unrelated-project.json').write_text('user project')

        def old_intact():
            assert (installed / 'version-marker.txt').read_text() == 'old'
            assert (installed / '.installed-files').read_bytes() == inventory
            assert len(list(plugins.glob('*.clap'))) == product['stock_plugins']
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, registry) as key:
                assert winreg.QueryValueEx(key, 'StockPlugins')[0] == 1

        collision = installed / 'skill/guides/added-by-upgrade.md'
        collision.write_text('private guide')
        install('new', 1)
        old_intact()
        assert collision.read_text() == 'private guide'
        collision.unlink()
        ancestor = installed / 'skill/new-folder'
        ancestor.write_text('private file blocks directory')
        install('new', 1)
        old_intact()
        assert ancestor.read_text() == 'private file blocks directory'
        ancestor.unlink()
        collision = plugins / 'UpgradeExtra-NOTICES.txt'
        collision.write_text('another host owns this filename')
        install('new', 1)
        old_intact()
        assert collision.read_text() == 'another host owns this filename'
        collision.unlink()
        install('new')  # Remember the selected component without /PLUGINS.
        assert (installed / 'version-marker.txt').read_text() == 'new'
        assert (plugins / 'notes.txt').read_text() == 'user plugin notes'
        assert (plugins / 'UpgradeExtra-NOTICES.txt').read_text() == 'new plugin notice'
        assert len(list(plugins.glob('*.clap'))) == product['stock_plugins']
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, registry) as key:
            assert winreg.QueryValueEx(key, 'StockPlugins')[0] == 1

        def uninstall(expected):
            result = subprocess.run([str(installed / 'uninstall.exe'), '/S', f'_?={installed}'])
            assert (result.returncode == 0) == (expected == 0), f'uninstaller exit {result.returncode}'

        def reject_junction(target, foreign_name, content):
            foreign = root / ('foreign-' + target.name)
            foreign.mkdir()
            (foreign / foreign_name).write_text(content)
            saved = target.with_name(target.name + '-saved')
            target.rename(saved)
            try:
                subprocess.run(['cmd', '/c', 'mklink', '/J', str(target), str(foreign)], check=True, capture_output=True)
                before = (installed / '.installed-files').read_bytes()
                uninstall(1)
                assert (foreign / foreign_name).read_text() == content
                assert (installed / '.installed-files').read_bytes() == before
                assert (installed / 'version-marker.txt').read_text() == 'new'
                assert (installed / 'skill/SKILL.md').exists()
            finally:
                if target.exists():
                    os.rmdir(target)  # Remove the junction itself, never its target.
                saved.rename(target)

        reject_junction(installed / 'skill', 'SKILL.md', 'foreign skill')
        reject_junction(plugins, next(plugins.glob('*.clap')).name, 'foreign host plugin')
        # A junction replacing the entire app root must also be refused.
        saved = root / 'installed-saved'
        installed.rename(saved)
        try:
            subprocess.run(['cmd', '/c', 'mklink', '/J', str(installed), str(saved)], check=True, capture_output=True)
            uninstall(1)
            assert (saved / '.installed-files').is_file()
            assert (saved / 'version-marker.txt').read_text() == 'new'
        finally:
            os.rmdir(installed)
            saved.rename(installed)
        uninstall(0)
        (installed / 'uninstall.exe').unlink(missing_ok=True)
        assert (installed / 'unrelated-project.json').read_text() == 'user project'
        assert (plugins / 'notes.txt').read_text() == 'user plugin notes'
        assert not list(plugins.glob('*.clap'))
        (plugins / 'notes.txt').unlink()
        plugins.rmdir()
        try:
            winreg.OpenKey(winreg.HKEY_CURRENT_USER, registry)
        except FileNotFoundError:
            pass
        else:
            raise AssertionError('Uninstall left ownership registry key')
    print('NSIS regressions passed: file/ancestor/plugin collisions preserve old install; selected plugins restored; app/skill/plugin junctions refused; unrelated files preserved')


if __name__ == '__main__':
    main()
