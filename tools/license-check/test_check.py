"""Regression tests for first-party license-gate coverage."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("license_gate", Path(__file__).with_name("check.py"))
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class BuildFiles(unittest.TestCase):
    def check_fixture(self, filename, contents):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / filename
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text(contents)
            (root / "THIRD_PARTY.md").write_text("Test fixture: no dependencies.\n")
            manifest = json.dumps({"allowed_licenses": ["MIT"], "dependencies": []})
            original_read = Path.read_text

            def read(path, *args, **kwargs):
                if path == root / "third_party/manifest.json":
                    return manifest
                return original_read(path, *args, **kwargs)

            # The fixture never creates or changes a third_party directory.
            with mock.patch.object(Path, "read_text", read), \
                    mock.patch.object(Path, "iterdir", return_value=iter(())), \
                    mock.patch.object(sys, "argv", ["check.py", "--root", str(root)]), \
                    contextlib.redirect_stderr(io.StringIO()) as errors:
                result = gate.main()
            return result, errors.getvalue()

    def test_undeclared_fetch_in_root_and_cmake_directory(self):
        for filename in ("CMakeLists.txt", "cmake/Dependencies.cmake", "libs/test/CMakeLists.txt"):
            with self.subTest(filename=filename):
                result, errors = self.check_fixture(filename, "FetchContent_Declare(undeclared)\n")
                self.assertEqual(result, 1)
                self.assertIn("fetches 'undeclared'", errors)

    def test_forbidden_spdx_in_root_build_file(self):
        result, errors = self.check_fixture("CMakeLists.txt", "# SPDX-License-" + "Identifier: GPL-3.0\n")
        self.assertEqual(result, 1)
        self.assertIn("SPDX identifier", errors)

    def test_only_the_named_file_may_name_a_gpl_tag(self):
        tag = 'string(REPLACE "SPDX-License-' + 'Identifier: MIT" "SPDX-License-' + 'Identifier: GPL-3.0-or-later")\n'
        result, errors = self.check_fixture("plugins/template/framework/tools/new_product.cmake", tag)
        self.assertEqual(result, 0, errors)
        result, errors = self.check_fixture("plugins/template/framework/tools/other.cmake", tag)
        self.assertEqual(result, 1)
        self.assertIn("SPDX identifier", errors)

    def test_cmake_commands_are_case_insensitive(self):
        for command in ("fetchcontent_declare", "externalproject_add"):
            with self.subTest(command=command):
                result, errors = self.check_fixture("libs/test/CMakeLists.txt", command + "(undeclared)\n")
                self.assertEqual(result, 1)
                self.assertIn("fetches 'undeclared'", errors)


class Expressions(unittest.TestCase):
    allowed = {"MIT", "BSD-3-Clause", "Apache-2.0", "Apache-2.0 WITH LLVM-exception"}

    def test_precedence_and_parentheses(self):
        for expression, expected in (
            ("MIT", True), ("GPL-3.0 AND (MIT OR BSD-3-Clause)", False),
            ("(GPL-3.0 AND MIT) OR BSD-3-Clause", True),
            ("GPL-3.0 AND MIT OR BSD-3-Clause", True),
            ("GPL-3.0 AND (MIT OR GPL-3.0)", False),
            ("((MIT))", True), ("MIT AND(BSD-3-Clause OR GPL-3.0)", True),
            ("MIT OR GPL-3.0 AND BSD-3-Clause", True),
        ):
            with self.subTest(expression=expression):
                self.assertEqual(gate.license_ok(expression, self.allowed), expected)

    def test_with_requires_an_allowed_complete_term(self):
        self.assertTrue(gate.license_ok("Apache-2.0 WITH LLVM-exception", self.allowed))
        self.assertFalse(gate.license_ok("MIT WITH Unknown-exception", self.allowed))
        self.assertFalse(gate.license_ok("GPL-3.0 WITH LLVM-exception", self.allowed))

    def test_malformed_syntax_is_rejected_even_in_allowed_alternatives(self):
        for expression in ("", "()", "MIT OR", "OR MIT", "MIT AND", "MIT MIT", "(MIT",
                           "MIT)", "MIT OR (BSD-3-Clause", "MIT OR )BSD-3-Clause(",
                           "MIT OR OR BSD-3-Clause", "MIT WITH", "MIT WITH X WITH Y",
                           "(MIT OR BSD-3-Clause) WITH X", "MIT OR GPL-3.0 garbage",
                           "MIT OR GPL-3.0@", "MIT or BSD-3-Clause",
                           "MIT OR GPL+3.0", "MIT OR foo:bar", "MIT OR LicenseRef-x+"):
            with self.subTest(expression=expression):
                self.assertFalse(gate.license_ok(expression, self.allowed))

    def test_current_manifest_expressions(self):
        manifest = json.loads((Path(__file__).resolve().parents[2] / "third_party/manifest.json").read_text())
        for dependency in manifest["dependencies"]:
            with self.subTest(dependency=dependency["name"]):
                self.assertTrue(gate.license_ok(dependency["license"], set(manifest["allowed_licenses"])))


if __name__ == "__main__":
    unittest.main()
