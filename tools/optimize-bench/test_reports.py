# SPDX-License-Identifier: GPL-3.0-or-later
"""Reporting regressions without launching an optimizer or audio device."""
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import bench
import summary


class Reports(unittest.TestCase):
    def test_even_seed_counts_use_the_median_and_name_the_mode(self):
        report = {"label": "example", "summary": {"modes": {"auto": {}, "module": {}}},
                  "tasks": {"synth.pad": {"tier": 1, "runs": [
                      {"mode": "auto", "best": 2}, {"mode": "auto", "best": 10},
                      {"mode": "module", "best": 4}, {"mode": "module", "best": 12}]}}}
        text = "\n".join(summary.task_table([report]))
        self.assertIn("6.000", text)
        self.assertIn("8.000", text)
        self.assertIn("auto", text)
        self.assertIn("module", text)

    def test_task_failures_are_visible_and_exit_nonzero(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(bench, "ROOT", root), \
                    patch.object(bench, "run_task", side_effect=RuntimeError("fixture failed")), \
                    redirect_stdout(StringIO()):
                code = bench.main(["--tasks", "pad", "--jobs", "1", "--out-dir", str(root / "reports")])
            self.assertNotEqual(code, 0)
            self.assertIn("fixture failed", (root / "reports/baseline.md").read_text())


if __name__ == "__main__":
    unittest.main()
