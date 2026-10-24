# SPDX-License-Identifier: GPL-3.0-or-later
"""Installer ownership/collision checks and strict dependency audit regressions."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import linux
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


class Dependencies(unittest.TestCase):
    def check(self, output):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "lib/example").mkdir(parents=True)
            (root / "app").write_bytes(b"\x7fELF")
            with patch("linux.subprocess.check_output", return_value=output):
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


if __name__ == "__main__":
    unittest.main()
