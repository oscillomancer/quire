#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render a stock plugin's factory samples from Marram, through the sandbox's
MCP server (the path an agent uses), as 16-bit FLAC. Usage:
  tools/factory-samples/render.py <recipes.json> [--out DIR] [--check]
      [--sandbox PATH] [--plugin PATH] [--flac PATH]
Defaults: build/dev binaries, and the folder holding recipes.json. --check
renders into a temporary folder and compares every file with the one there:
a render depends only on the recipe and the build, so the bytes match.

recipes.json: {"schema": "supple.factory-samples/1.0", "rate": 48000,
"sounds": [...]}. A sound is either a note, rendered from a Marram patch:
  {"name": "keys", "patch": {...}, "notation": "1: C4:w", "tempo": 60,
   "tail_s": 1.0}
or a sequence of other sounds, placed at beats and summed:
  {"name": "break", "tempo": 100, "beats": 8,
   "hits": [["kick", 0, 0.0], ["hat", 0.5, -6.0]]}   (sound, beat, dB)
and takes, after it is made: "channels" (1 sums to mono, 2 keeps stereo),
"seconds" (what is kept from the start), "fade_ms" (a cosine fade at the
end) and "peak_db" (normalized to that peak). An "internal" sound is used by
sequences and not written.
"""
import argparse, array, filecmp, json, math, os, re, shutil, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
with open(os.path.join(ROOT, "cmake/Product.cmake"), encoding="utf-8") as product:
    PRODUCT_SLUG = re.search(r'set\(PRODUCT_SLUG\s+"([a-z0-9-]+)"\)', product.read())[1]
EXE = ".exe" if os.name == "nt" else ""


class Mcp:
    def __init__(self, cmd, env):
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
        self.id = 0
        try:
            self.call_rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                          "clientInfo": {"name": "factory-samples", "version": "1"}})
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
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("the sandbox stopped")
            reply = json.loads(line)
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


def read_wav(path):
    """Planar float channels and the rate of a PCM or float WAV."""
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise RuntimeError(path + ": not a WAV")
    at, fmt, body = 12, None, None
    while at + 8 <= len(data):
        tag, size = data[at:at + 4], struct.unpack("<I", data[at + 4:at + 8])[0]
        chunk = data[at + 8:at + 8 + size]
        if tag == b"fmt ":
            fmt = chunk
        elif tag == b"data":
            body = chunk
        at += 8 + size + (size & 1)
    code, channels, rate = struct.unpack("<HHI", fmt[:8])
    bits = struct.unpack("<H", fmt[14:16])[0]
    if code == 0xFFFE:
        code = struct.unpack("<H", fmt[24:26])[0]
    width = bits // 8
    count = len(body) // width
    if code == 3 and bits == 32:
        values = array.array("f", body[:count * 4]).tolist()
    elif code == 3 and bits == 64:
        values = array.array("d", body[:count * 8]).tolist()
    elif code == 1 and bits in (16, 24, 32):
        scale = float(1 << (bits - 1))
        values = [int.from_bytes(body[i:i + width], "little", signed=True) / scale
                  for i in range(0, count * width, width)]
    else:
        raise RuntimeError(path + ": a WAV format this does not read")
    frames = len(values) // channels
    return [values[c:frames * channels:channels] for c in range(channels)], rate


def write_wav(path, channels, rate):
    """32-bit float."""
    frames = len(channels[0])
    interleaved = array.array("f", [channels[c][i] for i in range(frames) for c in range(len(channels))])
    body = interleaved.tobytes()
    head = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt " + \
        struct.pack("<IHHIIHH", 16, 3, len(channels), rate, rate * 4 * len(channels), 4 * len(channels), 32) + \
        b"data" + struct.pack("<I", len(body))
    with open(path, "wb") as f:
        f.write(head + body)


def finish(sound, channels, rate):
    """Its channel count, length, fade and peak, as the recipe says."""
    if sound.get("channels", 2) == 1 and len(channels) > 1:
        channels = [[sum(c[i] for c in channels) / len(channels) for i in range(len(channels[0]))]]
    elif len(channels) == 1 and sound.get("channels", 2) == 2:
        channels = [channels[0], list(channels[0])]
    if "seconds" in sound:
        keep = int(round(sound["seconds"] * rate))
        channels = [(c + [0.0] * max(0, keep - len(c)))[:keep] for c in channels]
    fade = int(round(sound.get("fade_ms", 0) * rate / 1000.0))
    frames = len(channels[0])
    for c in channels:
        for k in range(min(fade, frames)):
            c[frames - 1 - k] *= 0.5 - 0.5 * math.cos(math.pi * k / fade)
    if "peak_db" in sound:
        peak = max(abs(x) for c in channels for x in c)
        if peak > 0.0:
            gain = 10.0 ** (sound["peak_db"] / 20.0) / peak
            channels = [[x * gain for x in c] for c in channels]
    return channels


def render_note(mcp, instance, sound, rate):
    mcp.tool("patch_set", {"instance": instance, "patch": sound["patch"]})
    text = "notation 1\ntempo %g\n%s" % (sound.get("tempo", 60), sound["notation"])
    r = mcp.tool("render", {"notation": text, "tail_s": sound.get("tail_s", 1.0), "name": sound["name"]})
    channels, got = read_wav(r["path"])
    if got != rate:
        raise RuntimeError("%s rendered at %d Hz, not %d" % (sound["name"], got, rate))
    latency = int(r.get("latency_samples", 0))  # the note starts at the file's first frame
    return [c[latency:] for c in channels]


def sequence(sound, made, rate):
    beat = 60.0 / sound["tempo"]
    frames = int(round(sound["beats"] * beat * rate))
    out = None
    for name, at, db in sound["hits"]:
        hit = made[name]
        if out is None:
            out = [[0.0] * frames for _ in hit]
        start, gain = int(round(at * beat * rate)), 10.0 ** (db / 20.0)
        for c in range(len(out)):
            src = hit[min(c, len(hit) - 1)]
            for i in range(min(len(src), frames - start)):
                out[c][start + i] += gain * src[i]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("recipes")
    ap.add_argument("--out")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--sandbox", default=os.path.join(ROOT, "build/dev/host/sandbox", PRODUCT_SLUG + "-sandbox" + EXE))
    ap.add_argument("--plugin", default=os.path.join(ROOT, "build/dev/plugins/clap/Marram.clap"))
    ap.add_argument("--flac", default=os.path.join(ROOT, "build/dev/tools/factory-samples/factory-sample-flac" + EXE))
    a = ap.parse_args()
    book = json.load(open(a.recipes, encoding="utf-8"))
    if book.get("schema") != "supple.factory-samples/1.0":
        sys.exit(a.recipes + ": not supple.factory-samples/1.0")
    rate = int(book.get("rate", 48000))
    folder = a.out or os.path.dirname(os.path.abspath(a.recipes))
    work = tempfile.mkdtemp(prefix="factory-samples-")
    out = os.path.join(work, "out") if a.check else folder
    os.makedirs(out, exist_ok=True)
    home = os.path.join(work, "home")
    os.makedirs(home)
    env = dict(os.environ, HOME=home, USERPROFILE=home, APPDATA=os.path.join(home, "config"),
               LOCALAPPDATA=os.path.join(home, "local"), XDG_DATA_HOME=os.path.join(home, "data"))
    # A private home: nothing of the user's settings or presets reaches the renders.
    mcp = Mcp([a.sandbox, "--mcp", "--workdir", os.path.join(work, "sandbox"), "--plugin-path", a.plugin], env)
    written, made = [], {}
    try:
        mcp.tool("session_set", {"sample_rate": rate})
        instance = mcp.tool("plugin_load", {"plugin": a.plugin})["instance"]
        for sound in book["sounds"]:
            raw = sequence(sound, made, rate) if "hits" in sound else render_note(mcp, instance, sound, rate)
            made[sound["name"]] = finish(sound, raw, rate)
            if sound.get("internal"):
                continue
            wav = os.path.join(work, sound["name"] + ".wav")
            write_wav(wav, made[sound["name"]], rate)
            flac = os.path.join(out, sound["name"] + ".flac")
            subprocess.run([a.flac, wav, flac], check=True)
            written.append(sound["name"] + ".flac")
            print("%-12s %6.2f s  %3d KB" % (sound["name"], len(made[sound["name"]][0]) / rate,
                                             os.path.getsize(flac) // 1024))
    finally:
        mcp.close()
    differ = [n for n in written if a.check and not (os.path.exists(os.path.join(folder, n)) and
                                                    filecmp.cmp(os.path.join(out, n), os.path.join(folder, n), False))]
    shutil.rmtree(work, ignore_errors=True)
    for n in differ:
        print("differs from the committed file:", n)
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main())
