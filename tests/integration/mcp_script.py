#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run a Lua script over MCP: `exec` with script.run, its edits visible to a
later query, and a structured error when the script misbehaves."""
import argparse
import json
import os
import select
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("mode", choices=["engine", "sandbox"])
    args = parser.parse_args()
    if args.mode == "engine":
        # A script that edits, in one transaction, and returns what it made.
        code = ("local ids = {}\n"
                "q.batch('two tracks', function()\n"
                "  ids[#ids + 1] = q.track.add{name = 'Scripted A'}.track\n"
                "  ids[#ids + 1] = q.track.add{name = 'Scripted B'}.track\n"
                "end)\n"
                "print('made ' .. #ids)\n"
                "return {tracks = ids}")
        # The engine has the generic `query` tool (session.describe exists).
        follow_up = {"name": "query", "arguments": {"command": "track.list", "args": {}}}
    else:
        # The sandbox has no tracks: read the session through the script.
        code = ("local s = q.session.get()\n"
                "print('rate ' .. s.settings.sample_rate)\n"
                "return {rate = s.settings.sample_rate}")
        # The sandbox has no `query` tool: its dedicated session tool is name_get.
        follow_up = {"name": "session_get", "arguments": {}}

    with tempfile.TemporaryDirectory(prefix="supple-mcp-script-") as workdir:
        process = subprocess.Popen(
            [args.binary, "--mcp", "--workdir", workdir],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
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
            end = pending.index(b"\n")
            frame = bytes(pending[:end])
            del pending[: end + 1]
            return json.loads(frame)

        def send(message):
            os.write(process.stdin.fileno(),
                     (json.dumps(message, separators=(",", ":")) + "\n").encode())
            return receive()

        try:
            init = send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "protocolVersion": "2025-06-18", "capabilities": {},
                "clientInfo": {"name": "script-client", "version": "1.0"}}})
            assert init["result"]["protocolVersion"] == "2025-06-18", init
            os.write(process.stdin.fileno(), (
                json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"},
                           separators=(",", ":")) + "\n").encode())

            scripted = send({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {
                "name": "exec", "arguments": {"command": "script.run", "args": {
                    "name": "mcp-demo", "code": code, "budget_ms": 2000}}}})
            assert not scripted["result"].get("isError"), scripted
            out = scripted["result"]["structuredContent"]
            assert out["script"] == "mcp-demo", out
            assert out["instructions"] > 0, out
            assert isinstance(out["log"], list) and out["log"], out

            # What the script did is in the session, visible to any client.
            listed = send({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                           "params": follow_up})
            assert not listed["result"].get("isError"), listed
            seen = listed["result"]["structuredContent"]
            if args.mode == "engine":
                names = {track["name"] for track in seen["tracks"]}
                assert {"Scripted A", "Scripted B"} <= names, listed
                assert out["result"]["tracks"][0] == seen["tracks"][1]["id"], (out, seen)
            else:
                assert seen["settings"]["sample_rate"] == out["result"]["rate"], (out, seen)

            # A script that raises comes back as a structured command error.
            broken = send({"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {
                "name": "exec", "arguments": {"command": "script.run", "args": {
                    "code": "q.no_such_command()"}}}})
            assert broken["result"]["isError"] is True, broken
            assert broken["result"]["structuredContent"]["code"] == "script.error", broken

            syntax = send({"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {
                "name": "exec", "arguments": {"command": "script.run", "args": {
                    "code": "return 1 +"}}}})
            assert syntax["result"]["structuredContent"]["code"] == "script.syntax", syntax

            process.stdin.close()
            assert process.wait(timeout=10) == 0, process.stderr.read().decode()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
    print(f"MCP script.run ({args.mode}): exec, session, structured errors passed")


if __name__ == "__main__":
    main()
