#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4 acceptance: an LLM over MCP builds a small arrangement headlessly
and renders it.

A scripted MCP client drives the engine's stdio MCP server: tempo, three
tracks with Orrery patches (bass, pad, lead), notation-generated MIDI items,
a basic mix, a render, analysis of the render, and a save/reopen that
renders bit-identically. Every step's structured result is checked. With
--transcript, the exchange is written as a readable document (not a drift
test: plugin text formatting may change without the behaviour changing).

  mcp_arrangement.py <engine> <orrery.clap> [--transcript FILE]
"""

import argparse
import hashlib
import json
import os
import select
import subprocess
import tempfile
from pathlib import Path

BASS = {"schema": "supple.orrery-patch/1.0", "name": "Round bass", "seed": 1,
        "voice": {"polyphony": 1, "glide": "30 ms"},
        "oscillators": {"osc1": {"type": "va", "wave": "saw", "to": "filter1"},
                        "osc3": {"type": "sub", "wave": "sine", "octave": -1,
                                 "level": "-6 dB", "to": "filter1"}},
        "filters": {"filter1": {"type": "ladder", "cutoff": "400 Hz", "resonance": "20 %"}},
        "envelopes": {"amp": {"attack": "3 ms", "sustain": "90 %", "release": "80 ms"}}}
PAD = {"schema": "supple.orrery-patch/1.0", "name": "Warm pad", "seed": 2,
       "voice": {"polyphony": 8, "unison": 3, "unison_detune": "10 ct"},
       "oscillators": {"osc1": {"type": "va", "wave": "saw", "to": "filter1"}},
       "filters": {"filter1": {"type": "svf", "cutoff": "1.5 kHz"}},
       "envelopes": {"amp": {"attack": "300 ms", "sustain": "80 %", "release": "600 ms"}},
       "fx": {"chain": ["chorus", "reverb"]}}
LEAD = {"schema": "supple.orrery-patch/1.0", "name": "Bright lead", "seed": 3,
        "oscillators": {"osc1": {"type": "va", "wave": "square", "to": "filter1"}},
        "filters": {"filter1": {"type": "svf", "cutoff": "3 kHz"}},
        "envelopes": {"amp": {"attack": "5 ms", "sustain": "70 %", "release": "150 ms"}},
        "fx": {"chain": ["delay"]}}

BASS_LINE = "voice bass; 1: A1:q A1:q A1:e C2:e E2:q | F1:q F1:q F1:e A1:e C2:q | " \
            "G1:q G1:q G1:e B1:e D2:q | E1:q E1:q E1:h"
PAD_CHORDS = "voice pad; 1: [A2 C3 E3]:w | [F2 A2 C3]:w | [G2 B2 D3]:w | [E2 G#2 B2]:w"
LEAD_LINE = "voice lead; 1: E4:q A4:q C5:h | A4:q F4:q C5:h | B4:q G4:q D5:h | B4:w"


class Client:
    def __init__(self, argv):
        self.process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, bufsize=0)
        self.pending = bytearray()
        self.transcript = []
        self.next_id = 1

    def receive(self):
        while b"\n" not in self.pending:
            readable, _, _ = select.select([self.process.stdout], [], [], 120)
            if not readable:
                raise AssertionError("MCP server timed out")
            chunk = os.read(self.process.stdout.fileno(), 1 << 16)
            if not chunk:
                raise AssertionError("MCP server closed stdout: " +
                                     self.process.stderr.read().decode(errors="replace"))
            self.pending.extend(chunk)
        end = self.pending.index(b"\n")
        frame = bytes(self.pending[:end])
        del self.pending[: end + 1]
        return json.loads(frame)

    def send(self, method, params=None, notify=False):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        if not notify:
            message["id"] = self.next_id
            self.next_id += 1
        os.write(self.process.stdin.fileno(),
                 (json.dumps(message, separators=(",", ":")) + "\n").encode())
        return None if notify else self.receive()

    def call(self, tool, arguments, note):
        response = self.send("tools/call", {"name": tool, "arguments": arguments})
        result = response["result"]
        assert not result.get("isError"), (tool, result)
        structured = result["structuredContent"]
        self.transcript.append((note, tool, arguments, structured))
        return structured

    def close(self):
        self.process.stdin.close()
        code = self.process.wait(timeout=20)
        assert code == 0, self.process.stderr.read().decode(errors="replace")


def build(client):
    client.call("tempo_set", {"points": [{"beat": 0, "bpm": 100}]}, "Set the tempo.")
    steps = []
    for name, patch, notation, volume, pan in (
            ("Bass", BASS, BASS_LINE, -6, 0),
            ("Pad", PAD, PAD_CHORDS, -12, -20),
            ("Lead", LEAD, LEAD_LINE, -9, 25)):
        key = name.lower()
        steps += [
            {"command": "track.add", "args": {"name": name}, "as": key},
            {"command": "fx.add", "args": {"track": "${%s.track}" % key,
                                           "plugin": "com.suppleaudio.orrery", "patch": patch}},
            {"command": "item.add", "args": {"track": "${%s.track}" % key, "bar": 1,
                                             "notation": notation}},
            {"command": "track.set", "args": {"track": "${%s.track}" % key,
                                              "volume_db": volume, "pan": pan}},
        ]
    result = client.call("batch", {"label": "Build a four-bar arrangement", "steps": steps},
                         "Three tracks with Orrery patches, notation MIDI and a basic mix, "
                         "as one undo step.")
    assert len(result["results"]) == len(steps), result
    described = client.call("session_describe", {}, "Look at the session.")
    for name in ("Bass", "Pad", "Lead"):
        assert name in described["text"], described
    return described


def render(client, name):
    result = client.call("render_mix", {"name": name, "end_beat": 16, "tail_s": 1.0},
                         "Render the four bars with a one-second tail.")
    assert result["seconds"] > 9.0, result  # 16 beats at 100 BPM + tail
    assert -30.0 < result["peak_dbfs"] < 0.0, result
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("engine")
    parser.add_argument("orrery")
    parser.add_argument("--transcript", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="supple-mcp-arrangement-") as workdir:
        client = Client([args.engine, "--mcp", "--workdir", workdir,
                         "--plugin-path", args.orrery])
        try:
            init = client.send("initialize", {"protocolVersion": "2025-06-18",
                                              "capabilities": {},
                                              "clientInfo": {"name": "arrangement", "version": "1"}})
            assert "tools" in init["result"]["capabilities"], init
            client.send("notifications/initialized", notify=True)
            tools = {t["name"] for t in client.send("tools/list", {})["result"]["tools"]}
            for needed in ("batch", "render_mix", "session_describe", "project_save"):
                assert needed in tools, sorted(tools)
            build(client)
            first = render(client, "arrangement")
            if "analyze_render" in tools:
                analysis = client.call("analyze_render", {"render": first["render"]}
                                       if "render" in first else {"path": first["path"]},
                                       "Measure the render.")
                assert analysis, analysis
            client.call("project_save", {"path": "arrangement.json"}, "Save the project.")
            client.call("project_open", {"path": "arrangement.json"}, "Reopen it.")
            second = render(client, "reopened")
            digest = [hashlib.sha256(Path(r["path"]).read_bytes()).hexdigest()
                      for r in (first, second)]
            assert digest[0] == digest[1], "a reopened project must render bit-identically"
            client.close()
        finally:
            if client.process.poll() is None:
                client.process.kill()
                client.process.wait(timeout=5)
        if args.transcript:
            lines = ["# An arrangement over MCP", "",
                     "A scripted MCP client builds four bars with three Orrery tracks, "
                     "renders, saves, reopens and renders again (bit-identical). Paths "
                     "point into a temporary work directory.", ""]
            for note, tool, arguments, structured in client.transcript:
                text = json.dumps(structured, indent=2).replace(workdir, "<workdir>")
                lines += [f"## {tool}", "", note, "", "```json",
                          json.dumps(arguments, indent=2), "```", "", "Result:", "",
                          "```json", text, "```", ""]
            args.transcript.parent.mkdir(parents=True, exist_ok=True)
            args.transcript.write_text("\n".join(lines), encoding="utf-8")
    print("MCP arrangement: built, rendered, analyzed, saved, reopened, identical")


if __name__ == "__main__":
    main()
