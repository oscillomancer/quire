#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise a real sandbox socket server through the stdio MCP proxy."""
import json
import os
import select
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def main():
    server_binary, proxy_binary, target = sys.argv[1:4]
    engine_mode = target == "--engine"
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="supple-proxy-test-") as temporary:
        socket = Path(temporary) / "control.sock"
        command = [server_binary, "serve", "--socket", str(socket), "--workdir", temporary]
        if not engine_mode:
            command += ["--plugin-path", str(Path(target).parent)]
        server = subprocess.Popen(
            command,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        )
        proxy = None
        try:
            for _ in range(1200):  # up to 30 s: sanitizer builds start slowly
                if socket.exists():
                    break
                if server.poll() is not None:
                    raise AssertionError(server.stderr.read().decode())
                time.sleep(0.025)
            assert socket.exists(), "socket server did not start"
            proxy = subprocess.Popen(
                [proxy_binary, "--socket", str(socket)], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0,
            )
            pending = bytearray()

            def call(identifier, method, params):
                message = {"jsonrpc": "2.0", "id": identifier, "method": method, "params": params}
                os.write(proxy.stdin.fileno(), (json.dumps(message) + "\n").encode())
                while b"\n" not in pending:
                    readable, _, _ = select.select([proxy.stdout], [], [], 30)
                    assert readable, f"proxy timed out on {method}"
                    data = os.read(proxy.stdout.fileno(), 8192)
                    assert data, f"proxy exited on {method}: {proxy.stderr.read().decode()}"
                    pending.extend(data)
                index = pending.index(b"\n")
                result = json.loads(pending[:index])
                del pending[:index + 1]
                assert result["id"] == identifier, result
                return result

            init = call(1, "initialize", {"protocolVersion": "2025-06-18",
                                         "clientInfo": {"name": "proxy-test"}})
            assert init["result"]["protocolVersion"] == "2025-06-18", init
            listed = call(2, "tools/list", {})
            names = {tool["name"] for tool in listed["result"]["tools"]}
            if engine_mode:
                assert "session_describe" in names, names
                described = call(3, "tools/call", {"name": "session_describe", "arguments": {}})
                assert not described["result"].get("isError"), described
            else:
                assert {"plugin_load", "render"} <= names, names
                loaded = call(3, "tools/call", {"name": "plugin_load",
                                                "arguments": {"plugin": target}})
                assert not loaded["result"].get("isError"), loaded
                rendered = call(4, "tools/call", {"name": "render", "arguments": {
                    "notation": "voice lead; 1: C4:q r:h r:q", "tail_s": 0.0}})
                assert not rendered["result"].get("isError"), rendered
                assert Path(rendered["result"]["structuredContent"]["path"]).exists(), rendered
            proxy.stdin.close()
            assert proxy.wait(timeout=10) == 0, proxy.stderr.read().decode()
        finally:
            if proxy and proxy.poll() is None:
                proxy.kill()
                proxy.wait(timeout=5)
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)
            assert not socket.exists(), "server left its socket after shutdown"
    print(f"MCP proxy socket integration passed in {time.monotonic() - started:.2f}s")


if __name__ == "__main__":
    main()
