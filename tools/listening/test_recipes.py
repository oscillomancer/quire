# SPDX-License-Identifier: GPL-3.0-or-later
"""Failure cleanup without launching a plugin or audio device."""
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

import render_synth_recipes as recipes


class Cleanup(unittest.TestCase):
    def test_failed_handshake_reaps_child_and_closes_pipes(self):
        process = Mock()
        with patch.object(recipes.subprocess, "Popen", return_value=process), \
                patch.object(recipes.Mcp, "call_rpc", side_effect=RuntimeError("refused")):
            with self.assertRaisesRegex(RuntimeError, "refused"):
                recipes.Mcp(["unused"])
        process.stdin.close.assert_called_once()
        process.wait.assert_called_once_with(timeout=30)
        process.stdout.close.assert_called_once()

    def test_shutdown_timeout_kills_and_reaps_child(self):
        client = recipes.Mcp.__new__(recipes.Mcp)
        client.p = Mock()
        client.p.wait.side_effect = [subprocess.TimeoutExpired("fixture", 30), 0]
        client.close()
        client.p.kill.assert_called_once()
        self.assertEqual(client.p.wait.call_count, 2)
        client.p.stdout.close.assert_called_once()

    def test_tool_failure_still_closes_the_client(self):
        with tempfile.TemporaryDirectory() as directory:
            client = Mock()
            client.tool.side_effect = RuntimeError("plugin failed")
            with patch.object(recipes, "Mcp", return_value=client), \
                    patch.object(recipes.sys, "argv", ["recipes", "--out", directory]):
                with self.assertRaisesRegex(RuntimeError, "plugin failed"):
                    recipes.main()
            client.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
