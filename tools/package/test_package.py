# SPDX-License-Identifier: GPL-3.0-or-later
"""Installer ownership/collision checks and strict dependency audit regressions."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import linux
import windows
from common import ROOT


@unittest.skipUnless(os.name == "posix" and os.uname().sysname == "Linux", "Linux packaging")
class Installer(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="install-test-", dir="/tmp")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "download"
        self.prefix = self.root / 'prefix with spaces & dollars$'
        self.home = self.root / "home"
        self.home.mkdir()
        self.env = dict(os.environ, HOME=str(self.home))
        for rel in ("bin", "lib/example/plugins", "share/applications", "share/icons/hicolor/16x16/apps"):
            (self.source / rel).mkdir(parents=True)
        for name in ("example", "example-engine", "example-mcp", "example-bridge", "example-scanner"):
            (self.source / "bin" / name).write_text("program")
        (self.source / "lib/example/plugins/Stock.clap").write_text("plugin")
        (self.source / "lib/example/plugins/Stock-LICENSE.txt").write_text("notice")
        (self.source / "share/applications/example.desktop").write_text("[Desktop Entry]\nExec=example %f\n")
        (self.source / "share/icons/hicolor/16x16/apps/example.png").write_text("icon")
        for script in ("install", "uninstall"):
            text = (ROOT / f"tools/package/{script}.sh.in").read_text()
            target = self.source / f"{script}.sh"
            target.write_text(text.replace("@PRODUCT_SLUG@", "example").replace("@PRODUCT_NAME@", "Example"))
            target.chmod(0o755)

    def install(self, *extra):
        return subprocess.run([str(self.source / "install.sh"), "--prefix", str(self.prefix), *extra],
                              env=self.env, capture_output=True, text=True)

    def test_install_and_remove_only_owned_paths(self):
        other = self.home / ".clap/Other.clap"
        other.parent.mkdir()
        other.write_text("unrelated")
        result = self.install("--plugins")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.prefix / "bin/example-mcp").is_symlink())
        desktop = (self.prefix / "share/applications/example.desktop").read_text()
        self.assertIn(r'prefix with spaces & dollars\\$', desktop)
        self.assertTrue((self.home / ".clap/Stock.clap").is_file())
        result = subprocess.run([str(self.prefix / "opt/example/uninstall.sh"), "--prefix", str(self.prefix)],
                                env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.prefix.exists())
        self.assertEqual(other.read_text(), "unrelated")
        self.assertFalse((self.home / ".clap/Stock.clap").exists())
        self.assertTrue(self.root.exists(), "Uninstall must not prune above prefix")

    def test_collision_writes_nothing(self):
        conflict = self.home / ".clap/Stock.clap"
        conflict.parent.mkdir()
        conflict.write_text("existing")
        self.assertNotEqual(self.install("--plugins").returncode, 0)
        self.assertEqual(conflict.read_text(), "existing")
        self.assertFalse(self.prefix.exists())

    def test_plugins_are_opt_in(self):
        self.assertEqual(self.install().returncode, 0)
        self.assertFalse((self.home / ".clap").exists())

    def test_upgrade_replaces_owned_files_and_preserves_user_data(self):
        self.assertEqual(self.install("--plugins").returncode, 0)
        private_note = self.prefix / "opt/example/my-project.json"
        private_note.write_text("user project")
        (self.source / "bin/example").write_text("new version")
        (self.source / "lib/example/plugins/Stock.clap").write_text("new plugin")
        result = self.install("--plugins")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.prefix / "bin/example").read_text(), "new version")
        self.assertEqual((self.home / ".clap/Stock.clap").read_text(), "new plugin")
        self.assertEqual(private_note.read_text(), "user project")
        self.assertEqual(subprocess.run([str(self.prefix / "opt/example/uninstall.sh"), "--prefix", str(self.prefix)],
                                       env=self.env, capture_output=True).returncode, 0)
        self.assertEqual(private_note.read_text(), "user project")
        self.assertFalse((self.prefix / "bin/example").exists())

    def test_installed_installer_does_not_delete_itself(self):
        self.assertEqual(self.install().returncode, 0)
        result = subprocess.run([str(self.prefix / "opt/example/install.sh"), "--prefix", str(self.prefix) + "/."],
                                env=self.env, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.prefix / "bin/example").is_file())

    def test_uninstall_preserves_foreign_empty_icon_directories(self):
        unrelated = self.prefix / "share/icons/other-theme/32x32/apps"
        unrelated.mkdir(parents=True)
        self.assertEqual(self.install().returncode, 0)
        result = subprocess.run([str(self.prefix / "opt/example/uninstall.sh"), "--prefix", str(self.prefix)],
                                env=self.env, capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertTrue(unrelated.is_dir())
        self.assertFalse((self.prefix / "share/icons/hicolor").exists())


class Dependencies(unittest.TestCase):
    def check(self, output, symbols="", filename="app"):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "lib/example").mkdir(parents=True)
            (root / filename).write_bytes(b"\x7fELF")
            with patch("linux.subprocess.check_output", side_effect=lambda command, **kwargs: symbols if command[0] == "nm" else output):
                linux.check_dependencies(root, "example")

    def test_base_libraries(self):
        self.check("libc.so.6 => /lib/libc.so.6 (0x1)\n/lib64/ld-linux-x86-64.so.2 (0x2)\n")

    def test_undeclared_library(self):
        with self.assertRaisesRegex(RuntimeError, "unbundled dependency"):
            self.check("libsurprise.so.1 => /usr/lib/libsurprise.so.1 (0x1)\n")

    def test_missing_library(self):
        with self.assertRaisesRegex(RuntimeError, "unresolved"):
            self.check("libmissing.so.1 => not found\n")

    def test_jack_is_never_linked(self):
        with self.assertRaisesRegex(RuntimeError, "JACK"):
            self.check("libjack.so.0 => /usr/lib/libjack.so.0 (0x1)\n")

    def test_shared_cxx_runtimes_are_never_needed(self):
        for soname in ("libstdc++.so.6", "libgcc_s.so.1"):
            with self.subTest(soname=soname), self.assertRaisesRegex(RuntimeError, r"shared C\+\+ runtime"):
                self.check(f"{soname} => /usr/lib/{soname} (0x1)\n")

    def test_runtime_symbols_cannot_interpose(self):
        with self.assertRaisesRegex(RuntimeError, "runtime symbols are exported"):
            self.check("", "__cxa_throw T 123 45\n")

    def test_stock_entry_point_is_visible(self):
        with self.assertRaisesRegex(RuntimeError, "entry point"):
            self.check("", filename="Stock.clap")
        self.check("", "clap_entry D 123 45\n", filename="Stock.clap")


class WindowsManifest(unittest.TestCase):
    def test_uninstall_lists_only_staged_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            tree = root / "stage"
            (tree / "plugins/clap").mkdir(parents=True)
            (tree / "plugins/clap/Stock.clap").write_bytes(b"plugin")
            (tree / "app.exe").write_bytes(b"program")
            (root / "unrelated.txt").write_text("keep")
            windows.write_manifests(tree, root)
            manifest = (root / "app-files.nsh").read_text()
            self.assertIn('Delete "${root}\\app.exe"', manifest)
            self.assertIn('RMDir "${root}\\plugins\\clap"', manifest)
            self.assertNotIn("unrelated", manifest)
            self.assertNotIn("RMDir /r", manifest)
            self.assertNotIn('Delete "${root}\\*"', manifest)


if __name__ == "__main__":
    unittest.main()
