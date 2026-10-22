#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the stock synth's sound-design loop over both MCP deployments.

The committed Linux transcript is also a drift test. Only temporary paths,
render IDs, wall-clock timings, and long arrays are normalized in its display.
--portable runs both deployments and all raw structured-result assertions; the
Linux reference diff is informational because DSP values are platform-dependent.
"""

import argparse
import difflib
import json
import os
import re
from pipe_ready import wait_pipe
import subprocess
import tempfile
import time
from pathlib import Path

from processes import stop


PHRASE = "1: C3:h E3:q G3:q"
PATCH_TYPE = "supple.marram-patch/1.0"
PAD = {
    "schema": PATCH_TYPE,
    "name": "MCP demo pad",
    "params": {
        "filter1.type": "svf", "filter1.cutoff": "900 Hz", "filter1.env": "+0 oct",
        "amp.attack": "80 ms", "amp.release": "200 ms",
        "env2.attack": "100 ms", "env2.decay": "400 ms", "env2.sustain": "0 %",
    },
}
PROFILE = {
    "schema": "supple.analysis-profile/1.0",
    "targets": {"spectral.centroid_hz.median": {"target": 1500, "tolerance": 100}},
}
HEADER = """# The stock synth over MCP

This is a scripted sound-design session with Marram, the stock synth. The
same commands run directly in the sandbox and through its Unix socket proxy.
The client checks every step's structured result before recording the exchange.
Arrays longer than 12 entries show their count and names; temporary paths,
render IDs, and elapsed job times use stable placeholders. MCP text content
that exactly duplicates structuredContent is shown once.

"""


def displayed(value, workdir, synth, render_ids):
    """Normalize the documented volatile fields, preserving all other data."""
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            if key in ("duration_us", "time_ms"):
                continue
            if key == "seconds" and value.get("applied") is False:
                result[key] = "<elapsed-job-seconds>"
            elif key == "content" and "structuredContent" in value:
                result[key] = [{"type": "text", "text": "<same as structuredContent>"}]
            else:
                result[key] = displayed(item, workdir, synth, render_ids)
        return result
    if isinstance(value, list):
        if len(value) > 12:
            names = [item["name"] for item in value
                     if isinstance(item, dict) and isinstance(item.get("name"), str)]
            summary = {"_elided_array_count": len(value)}
            if names:
                summary["names"] = names
            return summary
        return [displayed(item, workdir, synth, render_ids) for item in value]
    if isinstance(value, str):
        stable = value.replace(str(workdir), "<workdir>").replace(str(synth), "<synth.clap>")
        def render_label(match):
            identifier = match.group()
            return render_ids.setdefault(identifier, f"<render-{len(render_ids) + 1}>")

        return re.sub(r"\brd_\d+\b", render_label, stable)
    return value


class Client:
    def __init__(self, process, transcript):
        self.process = process
        self.transcript = transcript
        self.pending = bytearray()
        self.next_id = 1

    def send(self, method, params):
        identifier = self.next_id
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": identifier, "method": method, "params": params}
        self.transcript.append(("request", request))
        os.write(self.process.stdin.fileno(),
                 (json.dumps(request, separators=(",", ":")) + "\n").encode())
        while b"\n" not in self.pending:
            readable = wait_pipe(self.process.stdout, 120)
            assert readable, f"MCP timed out on {method}"
            data = os.read(self.process.stdout.fileno(), 8192)
            assert data, f"MCP closed on {method}: {self.process.stderr.read().decode()}"
            self.pending.extend(data)
            assert len(self.pending) <= 16 * 1024 * 1024
        end = self.pending.index(b"\n")
        response = json.loads(self.pending[:end])
        del self.pending[:end + 1]
        assert response["id"] == identifier, response
        self.transcript.append(("response", response))
        return response

    def tool(self, name, arguments):
        response = self.send("tools/call", {"name": name, "arguments": arguments})
        assert "error" not in response, response
        result = response["result"]
        assert not result.get("isError"), (name, result)
        structured = result["structuredContent"]
        assert isinstance(structured, dict), response
        assert json.loads(result["content"][0]["text"]) == structured, response
        return structured


def session(client, synth):
    init = client.send("initialize", {
        "protocolVersion": "2025-06-18", "capabilities": {},
        "clientInfo": {"name": "synth-demo", "version": "1.0"},
    })
    assert init["result"]["protocolVersion"] == "2025-06-18", init
    assert "tools" in init["result"]["capabilities"], init
    listed = client.send("tools/list", {})["result"]["tools"]
    names = {tool["name"] for tool in listed}
    required = {"describe", "exec", "plugin_load", "plugin_describe", "patch_set", "patch_merge",
                "render", "analyze", "compare", "optimize_start"}
    assert required <= names, required - names

    described = client.tool("describe", {"command": "patch.merge"})
    assert "patch.merge" in json.dumps(described), described
    loaded = client.tool("plugin_load", {"plugin": str(synth)})
    instance = loaded["instance"]
    assert instance.startswith("in_") and loaded["semantic"], loaded
    details = client.tool("plugin_describe", {"instance": instance, "detail": "summary"})
    assert details["patch_type"] == PATCH_TYPE, details
    assert "filter1.cutoff" in json.dumps(details), details

    set_result = client.tool("patch_set", {"instance": instance, "patch": PAD})
    assert isinstance(set_result["description"], str) and set_result["description"]
    render_args = {"notation": PHRASE, "seconds": 2.0, "tail_s": 0.3,
                   "analyze": "summary"}
    first = client.tool("render", {**render_args, "name": "base pad"})
    assert first["render"].startswith("rd_") and Path(first["path"]).is_file(), first
    assert first["peak_dbfs"] > -100 and isinstance(first["analysis"], dict), first
    first_report = client.tool("analyze", {"source": first["render"], "detail": "standard"})
    assert "spectral" in first_report["report"], first_report

    changed = client.tool("patch_merge", {"instance": instance, "patch": {
        "params": {"filter1.cutoff": "1.4 kHz", "filter1.env": "+2 oct"},
        "routes": {"velocity>filter1.cutoff": {"amount": "+1 oct"}},
    }})
    assert changed["changed"], changed
    second = client.tool("render", {**render_args, "name": "opened pad"})
    assert second["render"] != first["render"] and Path(second["path"]).is_file(), second
    compared = client.tool("compare", {"reference": first["render"], "test": second["render"]})
    assert isinstance(compared["ltas_rms_db"], (int, float)), compared
    assert compared["ltas_rms_db"] > 0, compared

    started = client.tool("optimize_start", {
        "instance": instance, "params": ["filter1.cutoff"],
        "target": {"profile": PROFILE}, "phrase": PHRASE,
        "seconds": 2.0, "tail_s": 0.3, "evaluations": 20,
        "max_seconds": 120, "seed": 2,
    })
    assert started["status"] == "running" and started["job"].startswith("jb_"), started
    for _ in range(4):
        waited = client.tool("exec", {"command": "job.wait", "args": {
            "job": started["job"], "timeout_s": 30,
        }})
        if waited["status"] != "running":
            break
    assert waited["status"] == "done", waited
    result = waited["result"]
    assert result["loss"]["best"] <= result["loss"]["initial"], result
    assert "filter1.cutoff" in result["patch"]["params"], result
    assert result["applied"] is False, result
    merged = client.tool("patch_merge", {"instance": instance, "patch": result["patch"]})
    assert isinstance(merged["description"], str), merged
    final = client.tool("render", {**render_args, "name": "optimized pad"})
    assert Path(final["path"]).is_file(), final
    analyzed = client.tool("analyze", {"source": final["render"], "detail": "standard"})
    centroid = analyzed["report"]["spectral"]["centroid_hz"]["median"]
    assert isinstance(centroid, (int, float)) and abs(centroid - 1500) <= 100, analyzed
    assert len({first["render"], second["render"], final["render"]}) == 3


def run_mode(mode, sandbox, proxy_binary, synth, root):
    workdir = root / mode
    workdir.mkdir()
    socket = workdir / "control.sock"
    server = process = None
    transcript = []
    try:
        if mode == "stdio":
            command = [str(sandbox), "--mcp", "--workdir", str(workdir),
                       "--plugin-path", str(synth.parent)]
        else:
            server = subprocess.Popen(
                [str(sandbox), "serve", "--socket", str(socket), "--workdir", str(workdir),
                 "--plugin-path", str(synth.parent)],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            )
            for _ in range(400):
                if socket.exists():
                    break
                assert server.poll() is None, server.stderr.read().decode()
                time.sleep(0.025)
            assert socket.exists(), "socket server did not start"
            command = [str(proxy_binary), "--socket", str(socket)]
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, bufsize=0)
        session(Client(process, transcript), synth)
        process.stdin.close()
        assert process.wait(timeout=10) == 0, process.stderr.read().decode()
    finally:
        # The proxy or stdio sandbox first (it holds a connection), then the
        # socket server, which removes its socket on a graceful exit.
        stop(process, timeout=5, graceful=False)
        stop(server)
    # Windows TerminateProcess cannot run the server's endpoint destructor;
    # the harness removes this private directory after the demo finishes.
    if server is not None and os.name != "nt":
        assert not socket.exists(), "socket server left its socket"
    return transcript, workdir


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sandbox", type=Path)
    parser.add_argument("proxy", type=Path)
    parser.add_argument("synth", type=Path)
    parser.add_argument("transcript", type=Path)
    parser.add_argument("--write", action="store_true", help="regenerate the committed transcript")
    parser.add_argument("--portable", action="store_true",
                        help="check raw MCP contracts; print the Linux transcript diff for review")
    args = parser.parse_args()
    for path in (args.sandbox, args.proxy):
        assert path.is_file(), path
    assert args.synth.exists(), args.synth  # a macOS .clap is a bundle directory
    started = time.monotonic()
    sections = [HEADER]
    with tempfile.TemporaryDirectory(prefix="supple-synth-demo-") as temporary:
        root = Path(temporary)
        for mode in ("stdio", "proxy"):
            transcript, workdir = run_mode(mode, args.sandbox, args.proxy, args.synth, root)
            sections.append(f"## {mode}\n\n")
            render_ids = {}
            for direction, value in transcript:
                shown = displayed(value, workdir, args.synth, render_ids)
                sections.append(f"### {direction}\n\n```json\n"
                                f"{json.dumps(shown, indent=2, ensure_ascii=False)}\n```\n\n")
    content = "".join(sections)
    if args.write:
        args.transcript.parent.mkdir(parents=True, exist_ok=True)
        args.transcript.write_text(content, encoding="utf-8")
    else:
        expected = args.transcript.read_text(encoding="utf-8")
        if content != expected:
            difference = "".join(difflib.unified_diff(
                expected.splitlines(keepends=True), content.splitlines(keepends=True),
                fromfile="Linux reference", tofile="current platform"))
            if args.portable:
                print("Informational Linux-reference diff (raw contracts passed):")
                print(difference, end="")
            else:
                raise AssertionError("MCP transcript drift; regenerate with --write and review diff\n"
                                     + difference)
    print(f"Synth MCP demo passed in {time.monotonic() - started:.2f}s")


if __name__ == "__main__":
    main()
