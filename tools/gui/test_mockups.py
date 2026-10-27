# SPDX-License-Identifier: GPL-3.0-or-later
"""MCP transport smoke without a GUI, plugin or audio device."""
import sys
import unittest

import mockups


class Transport(unittest.TestCase):
    def test_pipe_round_trip_and_cleanup(self):
        code = "import sys; [print(line, end='', flush=True) for line in sys.stdin]"
        client = mockups.Mcp([sys.executable, "-u", "-c", code])
        try:
            reply = client.send("echo", {"text": "café"})
            self.assertEqual(reply["params"], {"text": "café"})
            self.assertEqual(reply["id"], 1)
            client.close()
            self.assertIsNotNone(client.process.poll())
            self.assertTrue(client.process.stdout.closed)
            self.assertTrue(client.process.stderr.closed)
        finally:
            if client.process.poll() is None:
                client.process.kill()
                client.process.wait()
            for stream in (client.process.stdin, client.process.stdout, client.process.stderr):
                stream.close()


if __name__ == "__main__":
    unittest.main()
