#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Identity mockups (P5.16): the real GUI, rendered offscreen in each theme.

Builds a demo session over the engine's MCP server (tempo ramp, markers and a
region, synth parts in a synth buss, a reverb return with sends, EQ and
compressor on the bass, automation, a bounced audio item), saves it, then
snapshots the app in several layouts per theme:

  mockups.py --build build/dev --out DIR [--theme dark|light|FILE ...] [--palette warm|ink ...]

Writes DIR/<palette>-<theme>-<view>.png and DIR/session/demo.json. Opens no
audio device.
"""

import argparse
import json
import os
import re
import select
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "integration"))
from mcp_arrangement import BASS, LEAD, PAD  # noqa: E402

SYNTH = "com.suppleaudio.marram"
ARP = {"schema": "supple.marram-patch/1.0", "name": "Glass arp",
       "params": {"osc1.wave": "triangle", "filter1.type": "svf", "filter1.cutoff": "2.4 kHz",
                  "amp.attack": "2 ms", "amp.sustain": "20 %", "amp.release": "120 ms"}}

BASS_LINE = ("voice bass; 1: A1:q A1:q A1:e C2:e E2:q | F1:q F1:q F1:e A1:e C2:q | "
             "G1:q G1:q G1:e B1:e D2:q | E1:q E1:q E1:h")
PAD_CHORDS = "voice pad; 1: [A2 C3 E3]:w | [F2 A2 C3]:w | [G2 B2 D3]:w | [E2 G#2 B2]:w"
LEAD_LINE = "voice lead; 1: E4:q A4:q C5:h | A4:q F4:q C5:h | B4:q G4:q D5:h | B4:w"
ARP_LINE = ("voice arp; 1: A3:s C4:s E4:s A4:s E4:s C4:s A3:s E3:s A3:s C4:s E4:s A4:s "
            "E4:s C4:s A3:s E3:s | F3:s A3:s C4:s F4:s C4:s A3:s F3:s C3:s F3:s A3:s C4:s "
            "F4:s C4:s A3:s F3:s C3:s | G3:s B3:s D4:s G4:s D4:s B3:s G3:s D3:s G3:s B3:s "
            "D4:s G4:s D4:s B3:s G3:s D3:s | E3:s G#3:s B3:s E4:s B3:s G#3:s E3:s B2:s "
            "E3:s G#3:s B3:s E4:s B3:s G#3:s E3:s B2:s")

# view name -> (--action arguments, the item selected); {bass-track} etc. are ids
VIEWS = {
    "arrange": ([], "bass-item"),
    "mixer": (["ui.mixer"], "bass-item"),
    "midi": (["ui.midi.edit"], "lead-item"),
    "fx": (['ui.fx.chain {"track": "{bass-track}"}'], "bass-item"),
    "history": (["ui.history"], "bass-item"),
    "routing": (["ui.routing"], "bass-item"),
    "render": (["ui.render"], "bass-item"),
}


def slug():
    text = (ROOT / "cmake/Product.cmake").read_text()
    return re.search(r'set\(PRODUCT_SLUG\s+"([a-z0-9-]+)"\)', text)[1]


class Mcp:
    def __init__(self, argv):
        self.process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, bufsize=0)
        self.pending = bytearray()
        self.next_id = 1

    def send(self, method, params=None, notify=False):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        if not notify:
            message["id"] = self.next_id
            self.next_id += 1
        os.write(self.process.stdin.fileno(),
                 (json.dumps(message, separators=(",", ":")) + "\n").encode())
        if notify:
            return None
        while b"\n" not in self.pending:
            if not select.select([self.process.stdout], [], [], 300)[0]:
                raise RuntimeError("MCP server timed out")
            chunk = os.read(self.process.stdout.fileno(), 1 << 16)
            if not chunk:
                raise RuntimeError(self.process.stderr.read().decode(errors="replace"))
            self.pending.extend(chunk)
        end = self.pending.index(b"\n")
        frame = json.loads(bytes(self.pending[:end]))
        del self.pending[: end + 1]
        return frame

    def call(self, tool, arguments):
        result = self.send("tools/call", {"name": tool, "arguments": arguments})["result"]
        if result.get("isError"):
            raise RuntimeError(f"{tool}: {json.dumps(result)[:2000]}")
        return result["structuredContent"]

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=30)


def step(command, args, key=None):
    s = {"command": command, "args": args}
    if key:
        s["as"] = key
    return s


def build_session(engine, plugin_dir, workdir):
    mcp = Mcp([str(engine), "--mcp", "--workdir", str(workdir), "--plugin-path", str(plugin_dir)])
    try:
        mcp.send("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                "clientInfo": {"name": "mockups", "version": "1"}})
        mcp.send("notifications/initialized", notify=True)
        mcp.call("tempo_set", {"points": [{"beat": 0, "bpm": 92}, {"beat": 48, "bpm": 92, "ramp": True},
                                          {"beat": 64, "bpm": 100}]})
        steps = [
            step("track.add", {"name": "Bass"}, "bass"),
            step("fx.add", {"track": "${bass.track}", "plugin": SYNTH, "patch": BASS}),
            step("fx.add", {"track": "${bass.track}", "plugin": "com.suppleaudio.yarrow"}),
            step("fx.add", {"track": "${bass.track}", "plugin": "com.suppleaudio.sundew"}),
            step("track.add", {"name": "Synths", "type": "buss"}, "synths"),
        ]
        for name, patch in (("Pad", PAD), ("Lead", LEAD), ("Arp", ARP)):
            key = name.lower()
            steps += [step("track.add", {"name": name, "parent": "${synths.track}"}, key),
                      step("fx.add", {"track": "${%s.track}" % key,
                                      "plugin": SYNTH, "patch": patch})]
        steps += [step("track.add", {"name": "Verb", "type": "buss"}, "verb"),
                  step("fx.add", {"track": "${verb.track}", "plugin": "com.suppleaudio.ghyll"}),
                  step("route.add", {"from": "${pad.track}", "to": "${verb.track}", "volume_db": -8}),
                  step("route.add", {"from": "${lead.track}", "to": "${verb.track}", "volume_db": -12})]
        items = []
        for key, line, bars in (("bass", BASS_LINE, (1, 5, 9, 13)), ("pad", PAD_CHORDS, (1, 5, 9, 13)),
                                ("arp", ARP_LINE, (5, 9, 13)), ("lead", LEAD_LINE, (9, 13))):
            for bar in bars:
                items.append(f"{key}-item-{bar}")
                steps.append(step("item.add", {"track": "${%s.track}" % key, "bar": bar,
                                               "notation": line, "name": key.capitalize()},
                                  items[-1].replace("-", "_")))
        for key, vol, pan in (("bass", -4, 0), ("pad", -14, -30), ("lead", -9, 20), ("arp", -16, 45),
                              ("synths", -2, 0), ("verb", -6, 0)):
            steps.append(step("track.set", {"track": "${%s.track}" % key, "volume_db": vol, "pan": pan}))
        for key, colour in (("bass", "red"), ("pad", "teal"), ("lead", "violet"), ("arp", "blue")):
            steps.append(step("track.set", {"track": "${%s.track}" % key, "color": colour}))
        steps += [
            step("marker.add", {"pos": 0, "name": "Intro"}),
            step("marker.add", {"pos": 16, "name": "Verse"}),
            step("marker.add", {"pos": 32, "name": "Build"}),
            step("region.add", {"start": 32, "end": 64, "name": "Chorus"}),
            step("env.add", {"track": "${pad.track}", "param": "volume",
                             "points": [{"bar": 1, "value": "-30 dB"}, {"bar": 5, "value": "-14 dB"},
                                        {"bar": 9, "value": "-10 dB"}, {"bar": 13, "value": "-6 dB"},
                                        {"bar": 16, "value": "-20 dB"}]}),
        ]
        result = mcp.call("batch", {"label": "Demo session", "steps": steps})
        ids = {}
        for s, r in zip(steps, result["results"]):
            if "as" in s:
                ids[s["as"]] = r
        bounce = mcp.call("render_mix", {"name": "bounce", "end_beat": 32, "tail_s": 0.5,
                                        "normalize": {"mode": "peak", "target": -1}})
        mcp.call("batch", {"label": "Bounce", "steps": [
            step("track.add", {"name": "Bounce", "index": 0}, "b"),
            step("track.set", {"track": "${b.track}", "color": "grey"}),
            step("item.add", {"track": "${b.track}", "bar": 1, "audio_file": bounce["path"],
                              "name": "bounce"})]})
        mcp.call("project_save", {"path": "demo.json"})
        mcp.close()
    finally:
        if mcp.process.poll() is None:
            mcp.process.kill()
            mcp.process.wait(timeout=5)
    return {"bass-item": ids["bass_item_1"]["item"], "lead-item": ids["lead_item_9"]["item"],
            "bass-track": ids["bass"]["track"]}


def snapshot(app, plugin_dir, project, theme, palette, view, ids, out, size):
    actions, selected = VIEWS[view]
    ui = {"panels": {"mixer": False, "history": False},
          "selection": {"tracks": [ids["bass-track"]], "items": [ids[selected]]},
          "arrange": {"start_seconds": 0, "pixels_per_second": 26, "scroll_y": 0, "row_height": 72}}
    project.with_suffix(".ui.json").write_text(json.dumps(ui, indent=2) + "\n")
    tag = Path(theme).stem if theme not in ("dark", "light") else theme
    png = out / f"{palette}-{tag}-{view}.png"
    argv = [str(app), str(project), "--offscreen", "--size", size, "--frames", "12",
            "--snapshot", str(png), "--no-socket", "--audio", "none", "--theme", theme,
            "--palette", palette, "--plugin-path", str(plugin_dir)]
    for a in actions:
        for key, value in ids.items():
            a = a.replace("{%s}" % key, value)
        argv += ["--action", a]
    subprocess.run(argv, check=True, timeout=180)
    return png


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path("build/dev"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--theme", action="append")
    parser.add_argument("--palette", action="append", choices=["warm", "ink"])
    parser.add_argument("--view", action="append", choices=sorted(VIEWS))
    parser.add_argument("--size", default="1600x1000")
    args = parser.parse_args()
    build = args.build.resolve()
    out = args.out.resolve()
    plugin_dir = build / "plugins" / "clap"
    session = out / "session"
    session.mkdir(parents=True, exist_ok=True)
    ids = build_session(build / "host" / "engine" / f"{slug()}-engine", plugin_dir, session)
    app = build / "daw" / "gui" / slug()
    for palette in args.palette or ["warm", "ink"]:
        for theme in args.theme or ["dark", "light"]:
            for view in args.view or list(VIEWS):
                print(snapshot(app, plugin_dir, session / "demo.json", theme, palette, view, ids,
                               out, args.size))


if __name__ == "__main__":
    main()
