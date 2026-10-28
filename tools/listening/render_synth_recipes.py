#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render every synth recipe in skill/guides/sound-design.md to a WAV for a
human listening check, through the sandbox's MCP server (the same path an
agent uses). Usage:
  tools/listening/render_synth_recipes.py [--sandbox PATH] [--plugin PATH] [--out DIR]
Defaults: build/release binaries and the stock synth, renders/listening/synth-recipes/.
"""
import argparse, json, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
with open(os.path.join(ROOT, "cmake/Product.cmake"), encoding="utf-8") as product:
    PRODUCT_SLUG = re.search(r'set\(PRODUCT_SLUG\s+"([a-z0-9-]+)"\)', product.read())[1]

# A phrase per recipe (by its bold title in the guide): what exercises it.
PHRASES = {
    "Warm pad": ("voice keys\n1: [C3 E3 G3 B3]:w | [A2 C3 E3 G3]:w", 3.0),
    "Plucky bass with a filter snap": ("voice bass\n1: C2:e r:e C2:e r:e Eb2:e r:e G1:e Bb1:e | C2:e r:e C2:e r:e F2:e r:e Eb2:q", 1.0),
    "Acid bass (legato, glide, resonant snap)": ("voice bass\n1: C2:s C2:s C3:s C2:s Eb2:s C2:s G2:s C2:s C2:s Bb1:s C2:s C3:s Eb2:s F2:s G2:s C2:s\n2 = 1", 0.5),
    "Sub bass (a clean fundamental)": ("voice bass\n1: C1:h G1:h | F1:h C1:h", 0.5),
    "Supersaw lead": ("voice lead\n1: C4:q E4:q G4:q C5:q | B4:h G4:h", 1.5),
    "Metallic bell (inharmonic partials)": ("voice bell\n1: C4:h G4:h | E5:w", 4.0),
    "FM electric piano": ("voice keys\n1: [C4 E4 G4]:h [D4 F4 A4]:h | [E4 G4 B4]:w", 2.0),
    "Noise snare": ("voice drum\n1: r:q D3:q r:q D3:q | r:q D3:q r:e D3:e D3:q", 0.5),
    "Vowel lead (a formant filter that moves)": ("voice lead\n1: G4:h A4:h | C5:w", 1.5),
    "String ensemble": ("voice strings\n1: [C3 G3 E4]:w | [F3 A3 C4]:w", 3.0),
}


class Mcp:
    def __init__(self, cmd):
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.id = 0
        try:
            self.call_rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                          "clientInfo": {"name": "listening-renders", "version": "1"}})
            self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        except BaseException:
            self.close()
            raise

    def send(self, msg):
        self.p.stdin.write(json.dumps(msg) + "\n")
        self.p.stdin.flush()

    def call_rpc(self, method, params):
        self.id += 1
        self.send({"jsonrpc": "2.0", "id": self.id, "method": method, "params": params})
        while True:
            reply = json.loads(self.p.stdout.readline())
            if reply.get("id") == self.id:
                if "error" in reply:
                    raise RuntimeError(json.dumps(reply["error"]))
                return reply["result"]

    def tool(self, name, args):
        r = self.call_rpc("tools/call", {"name": name, "arguments": args})
        if r.get("isError"):
            raise RuntimeError(r["content"][0]["text"])
        return r.get("structuredContent", {})

    def close(self):
        try:
            try:
                self.p.stdin.close()
            except BrokenPipeError:
                pass
            try:
                self.p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        finally:
            self.p.stdout.close()


def recipes():
    text = open(os.path.join(ROOT, "skill/guides/sound-design.md")).read()
    # A bold title, perhaps a line of notes, then the patch.
    for m in re.finditer(r"^\*\*([^*\n]+)\*\*[^\n]*\n(?:[^`\n][^\n]*\n)*```json\n(.*?)```", text,
                         re.S | re.M):
        title = m.group(1).strip()
        if title in PHRASES:
            yield title, json.loads(m.group(2))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sandbox", default=os.path.join(ROOT, "build/release/host/sandbox", PRODUCT_SLUG + "-sandbox"))
    ap.add_argument("--plugin", default=os.path.join(ROOT, "build/release/plugins/clap/Marram.clap"))
    ap.add_argument("--out", default=os.path.join(ROOT, "renders/listening/synth-recipes"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    work = os.path.join(a.out, ".work")
    mcp = Mcp([a.sandbox, "--mcp", "--workdir", work, "--plugin-path", a.plugin])
    done = 0
    try:
        loaded = mcp.tool("plugin_load", {"plugin": a.plugin})
        instance = loaded["instance"]
        for i, (title, patch) in enumerate(recipes(), 1):
            phrase, tail = PHRASES[title]
            mcp.tool("patch_set", {"instance": instance, "patch": patch})
            r = mcp.tool("render", {"notation": "notation 1\ntempo 100\n" + phrase, "tail_s": tail,
                                    "name": title, "analyze": "summary"})
            slug = re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-")
            dst = os.path.join(a.out, "%02d-%s.wav" % (i, slug))
            shutil.copyfile(r["path"] if "path" in r else r["wav"], dst)
            loud = r.get("analysis", {}).get("loudness", {}).get("integrated_lufs")
            print("%-34s %s  %s LUFS" % (title, os.path.relpath(dst, ROOT), loud))
            done += 1
    finally:
        mcp.close()
    shutil.rmtree(work, ignore_errors=True)
    return 0 if done == len(PHRASES) else 1


if __name__ == "__main__":
    sys.exit(main())
