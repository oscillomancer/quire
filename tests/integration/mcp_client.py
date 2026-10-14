#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drive one stdio MCP server through initialize, list, success and error calls."""
import argparse
import json
import os
import select
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("tool")
    parser.add_argument("--transcript", type=Path)
    args = parser.parse_args()
    transcript = []
    with tempfile.TemporaryDirectory(prefix="supple-mcp-test-") as workdir:
        process = subprocess.Popen(
            [args.binary, "--mcp", "--workdir", workdir],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            bufsize=0,
        )
        pending = bytearray()

        def receive():
            while b"\n" not in pending:
                readable, _, _ = select.select([process.stdout], [], [], 20)
                if not readable:
                    raise AssertionError("MCP server timed out")
                chunk = os.read(process.stdout.fileno(), 8192)
                if not chunk:
                    raise AssertionError("MCP server closed stdout early")
                pending.extend(chunk)
                assert len(pending) <= 16 * 1024 * 1024, "MCP response too large"
            end = pending.index(b"\n")
            frame = bytes(pending[:end])
            del pending[: end + 1]
            response = json.loads(frame)
            transcript.append(("response", response))
            return response

        def send(message, expect=True):
            transcript.append(("request", message))
            os.write(process.stdin.fileno(), (json.dumps(message, separators=(",", ":")) + "\n").encode())
            return receive() if expect else None

        try:
            init = send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "protocolVersion": "2025-06-18", "capabilities": {},
                "clientInfo": {"name": "integration-client", "version": "1.0"}}})
            assert init["result"]["protocolVersion"] == "2025-06-18", init
            assert "tools" in init["result"]["capabilities"], init
            send({"jsonrpc": "2.0", "method": "notifications/initialized"}, expect=False)
            listed = send({"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
            names = {tool["name"] for tool in listed["result"]["tools"]}
            assert args.tool in names, sorted(names)
            success = send({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {
                "name": args.tool, "arguments": {}}})
            result = success["result"]
            assert not result.get("isError"), success
            assert isinstance(result["structuredContent"], dict), success
            assert json.loads(result["content"][0]["text"]) == result["structuredContent"], success
            failure = send({"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {
                "name": args.tool, "arguments": {"misspelled": 1}}})
            assert failure["result"]["isError"] is True, failure
            assert failure["result"]["structuredContent"]["code"] == "command.invalid_args", failure
            assert "Hint:" in failure["result"]["content"][0]["text"], failure
            process.stdin.close()
            assert process.wait(timeout=10) == 0, process.stderr.read().decode()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
        if args.transcript:
            args.transcript.parent.mkdir(parents=True, exist_ok=True)
            args.transcript.write_text("\n\n".join(
                f"{direction}\n{json.dumps(message, indent=2, ensure_ascii=False)}"
                for direction, message in transcript) + "\n", encoding="utf-8")
    print(f"MCP {args.tool}: initialize, list, success, error passed")


if __name__ == "__main__":
    main()
