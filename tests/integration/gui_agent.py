#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""An agent drives the running GUI over its control socket (gui.md §1): the
GUI serves the command bus from its UI thread, so strips an agent adds through
the MCP proxy are in the session the window shows. The GUI runs offscreen and
writes its final frame as a PNG on exit."""
import json
import os
from pipe_ready import wait_pipe
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

from processes import stop


def close_gui(process):
    """Use the native close event on Windows, where terminate skips cleanup."""
    if os.name != "nt":
        process.terminate()
        return
    import ctypes
    from ctypes import wintypes
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    posted = []

    @callback_type
    def close(window, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == process.pid:
            posted.append(bool(user.PostMessageW(window, 0x0010, 0, 0)))  # WM_CLOSE
        return True

    user.EnumWindows(close, 0)
    assert any(posted), "the GUI has no native window to close"


def png_size_and_pixels(path):
    data = Path(path).read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    at, idat, width, height = 8, b"", 0, 0
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            width, height = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        at += 12 + length
    raw = zlib.decompress(idat)
    return width, height, raw


def main():
    gui_binary, proxy_binary, snapshot = sys.argv[1:4]
    # AF_UNIX is also the Windows transport; keep its endpoint below 108 bytes.
    with tempfile.TemporaryDirectory(prefix="sg-") as temporary:
        socket = Path(temporary) / "control.sock"
        gui = subprocess.Popen(
            [gui_binary, "--offscreen", "--socket", str(socket), "--workdir", temporary,
             "--snapshot", snapshot, "--audio", "dummy"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        proxy = None
        try:
            for _ in range(400):
                if socket.exists():
                    break
                if gui.poll() is not None:
                    raise AssertionError(gui.stderr.read().decode())
                time.sleep(0.025)
            assert socket.exists(), "the GUI did not serve its socket"
            proxy = subprocess.Popen([proxy_binary, "--socket", str(socket)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, bufsize=0)
            pending = bytearray()

            def call(identifier, method, params):
                message = {"jsonrpc": "2.0", "id": identifier, "method": method, "params": params}
                os.write(proxy.stdin.fileno(), (json.dumps(message) + "\n").encode())
                while b"\n" not in pending:
                    readable = wait_pipe(proxy.stdout, 30)
                    assert readable, f"proxy timed out on {method}"
                    chunk = os.read(proxy.stdout.fileno(), 65536)
                    assert chunk, f"proxy exited: {proxy.stderr.read().decode()}"
                    pending.extend(chunk)
                index = pending.index(b"\n")
                result = json.loads(pending[:index])
                del pending[:index + 1]
                assert result["id"] == identifier, result
                assert "error" not in result, result
                return result["result"]

            call(1, "initialize", {"protocolVersion": "2025-06-18",
                                   "clientInfo": {"name": "gui-agent-test"}})
            names = {tool["name"] for tool in call(2, "tools/list", {})["tools"]}
            assert "track_add" in names, sorted(names)[:20]
            for i, (kind, name) in enumerate([("buss", "Drums"), ("channel", "Kick")]):
                added = call(10 + i, "tools/call",
                             {"name": "track_add", "arguments": {"type": kind, "name": name}})
                assert not added.get("isError"), added
            described = call(20, "tools/call", {"name": "session_describe", "arguments": {}})
            assert not described.get("isError"), described
            text = json.dumps(described)
            assert "Drums" in text and "Kick" in text, text[:400]
            # A native window-close must not open the unsaved-changes prompt.
            saved = call(21, "tools/call", {"name": "project_save", "arguments": {
                "path": str(Path(temporary) / "song.json")}})
            assert not saved.get("isError"), saved
            proxy.stdin.close()
            assert proxy.wait(timeout=10) == 0, proxy.stderr.read().decode()
            close_gui(gui)
            assert gui.wait(timeout=20) == 0, gui.stderr.read().decode()
        finally:
            # Reap both children on every path, including native close failures.
            stop(proxy, timeout=5, graceful=False)
            code = stop(gui, timeout=20)
        assert code == 0, f"GUI exited {code}: {gui.stderr.read().decode()}"
    width, height, raw = png_size_and_pixels(snapshot)
    assert width >= 1280 and height >= 800 and width * 800 == height * 1280, (width, height)
    assert len(raw) == height * (width * 4 + 1)
    print(f"ok: agent added strips over the GUI's socket; final frame {width}x{height}")


if __name__ == "__main__":
    main()
