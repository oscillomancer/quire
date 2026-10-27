# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
"""Headless numeric arguments fail before opening any project or device."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = sys.argv.pop(1)


class NumericArguments(unittest.TestCase):
    def test_invalid_numbers(self):
        with tempfile.TemporaryDirectory(prefix="supple-cli-") as root:
            missing = str(Path(root) / "missing.json")
            for option, value in [
                ("bar", "1junk"), ("bar", "9" * 40), ("block", "128junk"),
                ("inputs", "0x"), ("outputs", "2.5"), ("rate", "48000oops"),
                ("seconds", "garbage"), ("seconds", "nan"), ("seconds", "inf"),
                ("seconds", "1e999"), ("seconds", "-1"), ("seconds", ""),
            ]:
                with self.subTest(option=option, value=value):
                    result = subprocess.run(
                        [BINARY, "play", missing, "--backend", "dummy", "--" + option, value],
                        capture_output=True, text=True, check=False,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    error = json.loads(result.stdout)
                    self.assertEqual(error["code"], "cli.number")
                    self.assertEqual(error["path"], "/" + option)
                    self.assertTrue(error["message"])
                    self.assertTrue(error["hint"])

    def test_valid_dummy_playback(self):
        with tempfile.TemporaryDirectory(prefix="supple-cli-") as root:
            project = str(Path(root) / "project.json")
            invocation = json.dumps({"command": "project.set", "args": {"name": "CLI test"}})
            subprocess.run(
                [BINARY, "exec", invocation, "--project", project, "--save", "--workdir", root],
                capture_output=True, text=True, check=True,
            )
            result = subprocess.run(
                [BINARY, "play", project, "--backend", "dummy", "--seconds", "0",
                 "--bar", "1", "--block", "128", "--rate", "4.8e4", "--inputs", "0",
                 "--outputs", "2"],
                capture_output=True, text=True, check=True,
            )
            self.assertEqual(json.loads(result.stdout)["backend"], "dummy")


if __name__ == "__main__":
    unittest.main()
