#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""On-demand, isolated REAPER CLAP acceptance. Python standard library only."""
import argparse
from array import array
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import random
import shlex
import shutil
import signal
import stat
import struct
import subprocess
import sys
import time
import uuid
import wave

ROOT = Path(__file__).resolve().parents[2]
RATE, SECONDS = 48000, 4


@dataclass(frozen=True)
class Case:
    artifact: str
    instrument: bool
    parameter: str
    first: float
    second: float
    expectation: str = "quieter"
    passthrough: bool = False
    display: str | None = None


def normalized(db, low, high):
    return (db - low) / (high - low)


# Continuous CLAP values are normalized curve positions (ADR-0010).
CASES = {
    "com.suppleaudio.probe-synth": Case("ProbeSynth", True, "Output Gain",
                                      normalized(-12, -60, 12), normalized(-24, -60, 12),
                                      "gain", display="Probe Synth"),
    "com.suppleaudio.orrery": Case("Orrery", True, "Output Level",
                                 normalized(0, -60, 12), normalized(-12, -60, 12), "gain"),
    "com.suppleaudio.utility": Case("Plumb", False, "Gain",
                                  normalized(0, -120, 24), normalized(-12, -120, 24), "gain", True),
    "com.suppleaudio.yarrow": Case("Yarrow", False, "Output", .5, .25, "gain", True),
    "com.suppleaudio.comp": Case("Ballast", False, "Threshold", 1, .4),
    "com.suppleaudio.analyzer": Case("Analyzer", False, "Hold", 0, 1, "transparent", True, "Gauge"),
    "com.suppleaudio.gate": Case("Gate", False, "Threshold", 0, .925, display="Sluice"),
    "com.suppleaudio.drive": Case("Kiln", False, "Output", .5, .25, "gain"),
    "com.suppleaudio.transient": Case("Flint", False, "Output", .5, .25, "gain", True),
    "com.suppleaudio.delay": Case("Echelon", False, "Output", .5, .25, "gain"),
}


class CheckError(RuntimeError):
    code = "check.failed"

    def structured(self, path=""):
        return {"code": self.code, "message": str(self), "path": path,
                "hint": "Inspect the saved REAPER log, metadata and project in the report directory."}


class ConfigChanged(CheckError):
    code = "config.changed"


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n")


def snapshot(root):
    """No atimes: reading the tree must not itself invalidate the guard."""
    result = {}
    if not root.exists():
        return result
    for path in [root, *sorted(root.rglob("*"))]:
        info = path.lstat()
        item = {"size": info.st_size, "mtime_ns": info.st_mtime_ns,
                "mode": stat.S_IFMT(info.st_mode)}
        if path.is_symlink():
            item["target"] = os.readlink(path)
        elif path.is_file():
            digest = hashlib.sha256()
            with path.open("rb") as source:
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(block)
            item["sha256"] = digest.hexdigest()
        result[str(path.relative_to(root))] = item
    return result


def changes(before, after):
    return sorted(key for key in before.keys() | after.keys() if before.get(key) != after.get(key))


class Runner:
    def __init__(self, executable, ini, user_config, directory, timeout, *, audio=False, block=64):
        self.executable, self.ini = executable, ini
        self.user_config, self.directory, self.timeout = user_config, directory, timeout
        self.commands = []
        self.audio = audio
        self.block = block

    def run(self, arguments, label):
        if self.audio and os.environ.get("SUPPLE_AUDIO_DEVICE_TEST") != "1":
            raise CheckError("real audio requires SUPPLE_AUDIO_DEVICE_TEST=1")
        command = [self.executable, "-newinst", "-nosplash", "-cfgfile", str(self.ini), *arguments]
        self.commands.append(command)
        print("$ " + shlex.join(command), flush=True)
        before = snapshot(self.user_config)
        write_json(self.directory / (label + ".config-before.json"), before)
        env = os.environ.copy()
        env["CLAP_PATH"] = str(self.ini.parent / "unused")  # ini path is authoritative.
        env["XDG_CONFIG_HOME"] = str(self.ini.parent / "xdg")
        env["LV2_PATH"] = str(self.ini.parent / "unused")
        env["VST_PATH"] = str(self.ini.parent / "unused")
        env["JACK_NO_START_SERVER"] = "1"
        if self.audio:
            # Ordinary client latency only; never force the shared graph.
            if "PIPEWIRE_QUANTUM" in env or "PIPEWIRE_RATE" in env:
                raise CheckError("unset PIPEWIRE_QUANTUM and PIPEWIRE_RATE before benchmarking")
            env["PIPEWIRE_LATENCY"] = f"{self.block}/48000"
        else:
            env["PIPEWIRE_REMOTE"] = "reaper-check-no-device"
            env["PULSE_SERVER"] = "unix:" + str(self.ini.parent / "no-pulse-server")
        process = None
        timed_out = False
        try:
            with (self.directory / (label + ".log")).open("wb") as log:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                           start_new_session=True, env=env)
                try:
                    code = process.wait(timeout=self.timeout)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    code = None
        finally:
            # Kill the entire new process group, even if the main process exited
            # but a scanner child remained. Never touch a pre-existing instance.
            if process is not None:
                for sig in (signal.SIGTERM, signal.SIGKILL):
                    try:
                        os.killpg(process.pid, sig)
                    except ProcessLookupError:
                        break
                    if sig == signal.SIGTERM:
                        try:
                            process.wait(timeout=1)
                        except subprocess.TimeoutExpired:
                            pass
                process.wait()
            after = snapshot(self.user_config)
            write_json(self.directory / (label + ".config-after.json"), after)
            changed = changes(before, after)
            if changed:
                raise ConfigChanged("USER CONFIG CHANGED: " + ", ".join(changed) +
                                    "; stopped without attempting to restore it")
        if timed_out:
            raise CheckError(f"{label}: timed out after {self.timeout:g}s; process group killed")
        if code != 0:
            raise CheckError(f"{label}: REAPER exit {code}; see {label}.log")


def private_config(directory, plugin_dir, user_config, *, block=256, device="dummy"):
    if device not in ("dummy", "jack"):
        raise CheckError("private config device must be dummy or jack")
    if device == "jack" and os.environ.get("SUPPLE_AUDIO_DEVICE_TEST") != "1":
        raise CheckError("real audio requires SUPPLE_AUDIO_DEVICE_TEST=1")
    cfg = directory / "config"
    cfg.mkdir(mode=0o700)
    (cfg / "unused").mkdir()
    license_file = user_config / "reaper-license.rk"
    if license_file.is_file():
        shutil.copyfile(license_file, cfg / license_file.name)
        (cfg / license_file.name).chmod(0o600)
    # Fresh allowlist, not a copy of the user's devices, projects, caches or paths.
    ini = cfg / "reaper.ini"
    ini.write_text(f"""[reaper]
linux_audio_mode={2 if device == "dummy" else 0}
linux_audio_srate=48000
linux_audio_bsize={block}
linux_audio_nch_in={0 if device == "dummy" else 2}
linux_audio_nch_out=2
clap_path_linux-x86_64={plugin_dir}
vstpath={cfg / "unused"}
vstpath64={cfg / "unused"}
lv2path_linux={cfg / "unused"}
loadlastproj=0
lastproject=
projecttabs=1
projecttab1=
verchk=0
midiins=0
midiins_all=0
midiins_cs=0
midiouts=0
[mididevcache]
[wave sink defaults]
""")
    ini.chmod(0o600)
    return ini


def test_signal(path):
    """Repeated 2 s pink-ish noise + 1 kHz tone and 125 Hz/8 kHz bursts."""
    rng = random.Random(481)
    states = [0.] * 12
    half = []
    for n in range(2 * RATE):
        # Voss octave sample-and-hold rows: original, no external DSP dependency.
        for row in range(len(states)):
            if n % (1 << row) == 0:
                states[row] = rng.uniform(-1, 1)
        t = n / RATE
        value = .018 * sum(states) + .15 * math.sin(2 * math.pi * 1000 * t)
        for start, end, hz in ((.2, .5, 125), (.7, 1.1, 8000)):
            if start <= t < end:
                value += .07 * math.sin(2 * math.pi * hz * t) * math.sin(math.pi * (t-start)/(end-start))**2
        half.append(round(max(-1, min(1, value)) * 32767))
    pcm = array("h", (x for value in half * 2 for x in (value, value)))
    if sys.byteorder != "little":
        pcm.byteswap()
    with wave.open(str(path), "wb") as output:
        output.setparams((2, 2, RATE, 0, "NONE", "not compressed"))
        output.writeframes(pcm.tobytes())


def lua_scope(statements):
    """Bound local lifetimes in generated tracks/FX (Lua limits active locals to 200)."""
    return ["do", *statements, "end"]


def lua_string(value):
    value = str(value)
    equals = ""
    while "]" + equals + "]" in value:
        equals += "="
    return "[" + equals + "[" + value + "]" + equals + "]"


def setup_script(case, plugin_id, folder, cfg, media):
    """REAPER writes the plugin chunk, opaque state and envelope addressing."""
    prefix = f"""local folder = {lua_string(folder)} .. '/'
local expected_resource = {lua_string(cfg)}
local fx_name = {lua_string(case.display or case.artifact)}
local parameter_name = {lua_string(case.parameter)}
local first, second = {case.first:.17g}, {case.second:.17g}
local log = assert(io.open(folder .. 'metadata.tsv', 'w'))
local function say(key, value)
  log:write(key .. '\\t' .. tostring(value):gsub('[\\r\\n\\t]', ' ') .. '\\n'); log:flush()
end
local function parameter(track, fx)
  for i=0,reaper.TrackFX_GetNumParams(track,fx)-1 do
    local _, name = reaper.TrackFX_GetParamName(track,fx,i)
    if name == parameter_name then return i end
  end
  error('parameter not found: ' .. parameter_name)
end
local function render_settings(name)
  reaper.GetSetProjectInfo_String(0,'RENDER_FILE',folder,true)
  reaper.GetSetProjectInfo_String(0,'RENDER_PATTERN',name,true)
  reaper.GetSetProjectInfo_String(0,'RENDER_FORMAT','ZXZhdxgAAA==',true)
  reaper.GetSetProjectInfo(0,'RENDER_SRATE',48000,true)
  reaper.GetSetProjectInfo(0,'RENDER_CHANNELS',2,true)
  reaper.GetSetProjectInfo(0,'RENDER_SETTINGS',0,true)
  reaper.GetSetProjectInfo(0,'RENDER_BOUNDSFLAG',0,true)
  reaper.GetSetProjectInfo(0,'RENDER_STARTPOS',0,true)
  reaper.GetSetProjectInfo(0,'RENDER_ENDPOS',4,true)
  reaper.GetSetProjectInfo(0,'RENDER_TAILFLAG',0,true)
  reaper.GetSetProjectInfo(0,'RENDER_NORMALIZE',0,true)
  reaper.GetSetProjectInfo(0,'RENDER_DITHER',0,true)
end
local function save(name)
  render_settings(name)
  reaper.Main_SaveProjectEx(0,folder .. name .. '.RPP',8)
end
local ok, err = xpcall(function()
  say('version',reaper.GetAppVersion())
  say('resource',reaper.GetResourcePath())
  assert(reaper.GetResourcePath() == expected_resource, 'private resource directory not selected')
  local opened, mode = reaper.GetAudioDeviceInfo('MODE')
  say('audio_mode',opened and mode or 'closed (Dummy configured)')
  assert(not opened or mode:lower():find('dummy'), 'expected Dummy audio; refusing to continue')
  reaper.Audio_Quit()
  reaper.Main_OnCommand(40023,0)
  reaper.GetSetProjectInfo(0,'PROJECT_SRATE',48000,true)
  reaper.GetSetProjectInfo(0,'PROJECT_SRATE_USE',1,true)
  reaper.InsertTrackAtIndex(0,true)
  local track = reaper.GetTrack(0,0)
  reaper.SetMediaTrackInfo_Value(track,'I_NCHAN',2)
  reaper.SetMediaTrackInfo_Value(track,'I_AUTOMODE',1)
  local fx = reaper.TrackFX_AddByName(track,'CLAP:' .. fx_name,false,-1)
  assert(fx >= 0, 'CLAP not found: ' .. fx_name)
  local _, ident = reaper.TrackFX_GetNamedConfigParm(track,fx,'fx_ident')
  say('fx_ident',ident)
  assert(ident:find({lua_string(plugin_id)},1,true), 'loaded unexpected plugin identity')
  local index = parameter(track,fx)
  local _, pid = reaper.TrackFX_GetParamIdent(track,fx,index)
  say('parameter_index',index); say('parameter_ident',pid)
  say('parameter_name',parameter_name)
  for i=0,reaper.TrackFX_GetNumParams(track,fx)-1 do
    local _, name = reaper.TrackFX_GetParamName(track,fx,i)
    local _, ident = reaper.TrackFX_GetParamIdent(track,fx,i)
    local value, low, high = reaper.TrackFX_GetParam(track,fx,i)
    say('param.' .. i,name .. ' | ' .. ident .. ' | ' .. value .. ' | ' .. low .. ' | ' .. high)
  end
"""
    if case.instrument:
        source = """
  local item = reaper.CreateNewMIDIItemInProj(track,0,4,false)
  local take = reaper.GetActiveTake(item)
  for half=0,1 do
    for j,note in ipairs({48,55,60,64}) do
      local t = half*2+(j-1)*.5
      local a = reaper.MIDI_GetPPQPosFromProjTime(take,t)
      local b = reaper.MIDI_GetPPQPosFromProjTime(take,t+.4)
      reaper.MIDI_InsertNote(take,false,false,a,b,0,note,100,true)
    end
  end
  reaper.MIDI_Sort(take)
"""
    else:
        source = f"""
  local item = reaper.AddMediaItemToTrack(track)
  local take = reaper.AddTakeToMediaItem(item)
  local source = reaper.PCM_Source_CreateFromFile({lua_string(media)})
  assert(source, 'source WAV not found')
  reaper.SetMediaItemTake_Source(take,source)
  reaper.SetMediaItemInfo_Value(item,'D_LENGTH',4)
  reaper.SetMediaItemInfo_Value(item,'D_FADEINLEN',0)
  reaper.SetMediaItemInfo_Value(item,'D_FADEOUTLEN',0)
"""
    suffix = """
  save('default')
  reaper.TrackFX_SetEnabled(track,fx,false); save('bypass')
  reaper.TrackFX_SetEnabled(track,fx,true)
  assert(reaper.TrackFX_SetParam(track,fx,index,second))
  -- REAPER queues CLAP parameter edits until processing/flush; render before
  -- saving so the blob contains the delivered value, not just its UI cache.
  render_settings('state-apply')
  reaper.Main_OnCommand(42230,0)
  save('state')
  reaper.Main_openProject('noprompt:' .. folder .. 'state.RPP')
  track = reaper.GetTrack(0,0); fx = 0; index = parameter(track,fx)
  local value = reaper.TrackFX_GetParam(track,fx,index)
  say('state_readback',value)
  assert(math.abs(value-second) < 1e-8, 'non-default state did not survive reload')
  save('state-roundtrip')
  assert(reaper.TrackFX_SetParam(track,fx,index,first))
  local env = reaper.GetFXEnvelope(track,fx,index,true)
  assert(env, 'cannot create parameter envelope')
  reaper.DeleteEnvelopePointRange(env,-1,10)
  for _,point in ipairs({{0,first},{2,second},{4,second}}) do
    assert(reaper.InsertEnvelopePoint(env,point[1],point[2],1,0,false,true))
  end
  reaper.Envelope_SortPoints(env)
  local _, chunk = reaper.GetEnvelopeStateChunk(env,'',false)
  say('envelope_header',chunk:match('[^\\r\\n]+'))
  save('automation')
  reaper.Main_openProject('noprompt:' .. folder .. 'automation.RPP')
  track = reaper.GetTrack(0,0); index = parameter(track,0)
  env = reaper.GetFXEnvelope(track,0,index,false)
  assert(env and reaper.CountEnvelopePoints(env) == 3, 'envelope did not survive reload')
  for i,expected in ipairs({{0,first},{2,second},{4,second}}) do
    local found,t,v = reaper.GetEnvelopePoint(env,i-1)
    assert(found and math.abs(t-expected[1]) < 1e-8 and math.abs(v-expected[2]) < 1e-8,
           'saved envelope point mismatch')
  end
  say('envelope_reload','pass')
  say('complete','1')
end,debug.traceback)
if not ok then say('error',err) end
log:close()
-- All projects were already saved; avoid a save dialog when closing.
reaper.Main_openProject('noprompt:' .. folder .. 'empty.RPP')
reaper.defer(function() reaper.Main_OnCommand(40004,0) end)
"""
    return prefix + source + suffix


def editor_script(plugin_id, folder, cfg):
    """Pump REAPER's UI through defer; never synthesize desktop input."""
    return f"""local folder = {lua_string(folder)} .. '/'
local expected_resource = {lua_string(cfg)}
local log = assert(io.open(folder .. 'editor.tsv', 'w'))
local track, fx = nil, 0
local done, phase, turns = false, 'opening', 0
local deadline = 0
local function say(key,value)
  log:write(key .. '\\t' .. tostring(value):gsub('[\\r\\n\\t]', ' ') .. '\\n'); log:flush()
end
local function finish(err)
  if done then return end
  done = true
  if err then say('error',err) else say('complete','1') end
  -- Close even when confirmation fails. The outer runner kills this entire
  -- instance if any host call hangs before we can reach the deferred quit.
  if track then
    reaper.TrackFX_Show(track,fx,2)
    reaper.TrackFX_Show(track,fx,0)
  end
  log:close()
  reaper.Main_openProject('noprompt:' .. folder .. 'empty.RPP')
  reaper.defer(function() reaper.Main_OnCommand(40004,0) end)
end
local function poll()
  local ok,err = xpcall(function()
    turns = turns + 1
    assert(reaper.time_precise() <= deadline, 'editor ' .. phase .. ' confirmation timed out')
    local floating = reaper.TrackFX_GetFloatingWindow(track,fx) ~= nil
    local opened = reaper.TrackFX_GetOpen(track,fx)
    if phase == 'opening' and floating and opened and turns >= 2 then
      say('shown','1')
      reaper.TrackFX_Show(track,fx,2)
      reaper.TrackFX_Show(track,fx,0)
      phase,turns = 'closing',0
    elseif phase == 'closing' and not floating and not opened then
      say('closed','1')
      finish()
      return
    end
    reaper.defer(poll)
  end,debug.traceback)
  if not ok then finish(err) end
end
local ok,err = xpcall(function()
  say('version',reaper.GetAppVersion())
  assert(reaper.GetResourcePath() == expected_resource, 'private resource directory not selected')
  local opened,mode = reaper.GetAudioDeviceInfo('MODE')
  say('audio_mode',opened and mode or 'closed (Dummy configured)')
  assert(not opened or mode:lower():find('dummy'), 'expected Dummy audio; refusing to continue')
  reaper.Audio_Quit()
  reaper.Main_openProject('noprompt:' .. folder .. 'default.RPP')
  track = reaper.GetTrack(0,0)
  assert(track, 'saved project has no track')
  local _,ident = reaper.TrackFX_GetNamedConfigParm(track,fx,'fx_ident')
  assert(ident:find({lua_string(plugin_id)},1,true), 'loaded unexpected plugin identity')
  say('fx_ident',ident)
  deadline = reaper.time_precise() + 3
  reaper.TrackFX_Show(track,fx,3)
  reaper.defer(poll)
end,debug.traceback)
if not ok then finish(err) end
"""


def check_editor(plugin_id, runner, folder, cfg):
    script = folder / "editor.lua"
    script.write_text(editor_script(plugin_id, folder, cfg))
    runner.run([str(script)], folder.name + "-editor")
    metadata = read_metadata(folder / "editor.tsv", "editor")
    if "error" in metadata or any(metadata.get(key) != "1" for key in ("shown", "closed", "complete")):
        raise CheckError(metadata.get("error", "editor was not confirmed shown and closed"))
    return metadata


def read_metadata(path, context="setup"):
    if not path.exists():
        raise CheckError(f"{context} did not produce metadata; see {context} log (display/license/script startup)")
    return dict(line.split("\t", 1) for line in path.read_text().splitlines() if "\t" in line)


def read_wav(path):
    """Read PCM or IEEE float RIFF, including WAVE_FORMAT_EXTENSIBLE."""
    if path.stat().st_size > 16 * 1024 * 1024:
        raise CheckError(f"{path.name}: oversized WAV for a four-second stereo render")
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise CheckError(f"{path.name}: not a RIFF WAV")
    pos, fmt, audio = 12, None, None
    while pos + 8 <= len(data):
        tag, size = struct.unpack_from("<4sI", data, pos)
        pos += 8
        chunk = data[pos:pos+size]
        if len(chunk) != size:
            raise CheckError("truncated WAV chunk")
        if tag == b"fmt ":
            fmt = chunk
        elif tag == b"data":
            audio = chunk
        pos += size + size % 2
    if fmt is None or audio is None or len(fmt) < 16:
        raise CheckError("missing WAV fmt/data")
    encoding, channels, rate, _, align, bits = struct.unpack_from("<HHIIHH", fmt)
    if encoding == 65534 and len(fmt) >= 40:
        encoding = struct.unpack_from("<H", fmt, 24)[0]
    if channels != 2 or rate != RATE or align <= 0 or align != channels * (bits // 8):
        raise CheckError(f"unexpected WAV format: channels={channels}, rate={rate}, bits={bits}")
    frames = len(audio) // align
    if len(audio) % align or frames != RATE * SECONDS:
        raise CheckError(f"WAV length {frames} frames; expected {RATE * SECONDS}")
    if encoding == 3 and bits in (32, 64):
        samples = array("f" if bits == 32 else "d")
        samples.frombytes(audio)
        if sys.byteorder != "little":
            samples.byteswap()
    elif encoding == 1 and bits in (8, 16, 24, 32):
        width = bits // 8
        if width == 1:
            samples = array("d", ((x - 128) / 128 for x in audio))
        else:
            samples = array("d", (int.from_bytes(audio[i:i+width], "little", signed=True) /
                                  (1 << (bits-1)) for i in range(0, len(audio), width)))
    else:
        raise CheckError(f"unsupported WAV encoding {encoding}/{bits}")
    if not all(math.isfinite(x) for x in samples):
        raise CheckError("non-finite WAV samples")
    return samples


def rms(samples, start, end):
    values = samples[round(start * RATE) * 2:round(end * RATE) * 2]
    return math.sqrt(sum(x*x for x in values) / len(values))


def difference(a, b):
    return math.sqrt(sum((x-y)**2 for x, y in zip(a, b)) / len(a))


def automation_delta(automated, reference):
    # Relative to the same windows in an unautomated render: cancels note/tail
    # differences and detector startup, rather than assuming stationary media.
    ratios = [rms(automated, a, b) / max(1e-15, rms(reference, a, b))
              for a, b in ((.35, 1.8), (2.35, 3.8))]
    return 20 * math.log10(max(1e-15, ratios[1]) / max(1e-15, ratios[0]))


def check_plugin(plugin_id, case, runner, folder, cfg, media, editor=False):
    row = {"plugin": plugin_id, "load": "fail", "signal": "skipped",
           "automation": "skipped", "state": "skipped", "editor": "skipped",
           "editor_reason": "default project did not load" if editor else "--editor not requested",
           "reason": "", "metrics": {}}
    folder.mkdir()
    (folder / "empty.RPP").write_text('<REAPER_PROJECT 0.1 "7" 0\n>\n')
    script = folder / "setup.lua"
    script.write_text(setup_script(case, plugin_id, folder, cfg, media))
    stage = "load"
    try:
        runner.run([str(script)], case.artifact + "-setup")
        metadata = read_metadata(folder / "metadata.tsv")
        row["metadata"] = metadata
        if "error" in metadata or metadata.get("complete") != "1":
            raise CheckError(metadata.get("error", "incomplete setup"))
        # State check is a non-default value, reloaded by REAPER before resaving.
        row["state"] = "pass"
        renders = {}
        for name in ("default", "bypass", "automation"):
            stage = {"default": "load", "bypass": "signal", "automation": "automation"}[name]
            runner.run(["-renderproject", str(folder / (name + ".RPP"))], case.artifact + "-" + name)
            output = folder / (name + ".wav")
            if not output.is_file():
                raise CheckError(f"{name}: output WAV absent")
            renders[name] = read_wav(output)
            if name == "default":
                row["load"] = "pass"
        stage = "signal"
        audio, bypass, automated = (renders[x] for x in ("default", "bypass", "automation"))
        level, residual = rms(audio, .35, 3.8), difference(audio, bypass)
        row["metrics"].update(rms=level, bypass_residual_rms=residual)
        if level <= 1e-5:
            row["signal"] = "fail"
            raise CheckError("default render is silent")
        if case.instrument:
            correct = rms(bypass, .35, 3.8) < 1e-7
        elif case.passthrough:
            correct = audio == bypass
        else:
            correct = residual > 1e-6
        row["signal"] = "pass" if correct else "fail"
        if not correct:
            raise CheckError("default/bypass comparison did not match expectation")
        delta = automation_delta(automated, audio)
        stage = "automation"
        row["metrics"]["automation_delta_db"] = delta
        if case.expectation == "transparent":
            correct = automated == bypass
            row["reason"] = "Hold envelope/save/reload and audio transparency checked; meter freeze not measured"
        else:
            correct = -13 <= delta <= -11 if case.expectation == "gain" else delta < -3
        row["automation"] = "pass" if correct else "fail"
        if not correct:
            raise CheckError(f"automation delta {delta:.3f} dB did not match {case.expectation}")
    except ConfigChanged:
        raise
    except (CheckError, OSError, ValueError) as error:
        row[stage] = "fail"
        row["reason"] = str(error)
        structured = error if isinstance(error, CheckError) else CheckError(str(error))
        row["error"] = structured.structured(str(folder))
        if (folder / "metadata.tsv").exists():
            row["metadata"] = read_metadata(folder / "metadata.tsv")
    if editor and row["load"] == "pass":
        try:
            row["editor_metadata"] = check_editor(plugin_id, runner, folder, cfg)
            row["editor"] = "pass"
            row["editor_reason"] = "floating editor confirmed shown and closed; screenshot not captured"
        except ConfigChanged:
            raise
        except (CheckError, OSError, ValueError) as error:
            row["editor"] = "fail"
            row["editor_reason"] = str(error)
            row["reason"] = "; ".join(filter(None, (row["reason"], "editor: " + str(error))))
            structured = error if isinstance(error, CheckError) else CheckError(str(error))
            row["editor_error"] = structured.structured(str(folder))
    return row


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/dev")
    parser.add_argument("--reaper", default=shutil.which("reaper"))
    parser.add_argument("--timeout", type=float, default=20)
    parser.add_argument("--plugin", action="append", choices=sorted(CASES))
    parser.add_argument("--editor", action="store_true",
                        help="also confirm each floating editor is shown and closed (requires a display)")
    args = parser.parse_args(argv)
    if not args.reaper or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("REAPER executable and positive finite timeout required")
    plugin_dir = args.build_dir.resolve() / "plugins/clap"
    inventory = sorted(plugin_dir.glob("*.clap"))
    if not inventory:
        parser.error(f"no CLAP artifacts in {plugin_dir}; build the selected preset first")
    directory = ROOT / "build/reaper-check" / (time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8])
    directory.mkdir(parents=True, mode=0o700)
    user_config = Path.home() / ".config/REAPER"
    before = snapshot(user_config)
    write_json(directory / "suite.config-before.json", before)
    results, runner = [], None
    fatal = None
    fatal_error = None
    try:
        ini = private_config(directory, plugin_dir, user_config)
        runner = Runner(args.reaper, ini, user_config, directory, args.timeout)
        media = directory / "signal.wav"
        test_signal(media)
        by_artifact = {case.artifact: (key, case) for key, case in CASES.items()}
        seen = set()
        for artifact in inventory:
            pair = by_artifact.get(artifact.stem)
            if pair is None:
                if not args.plugin:
                    results.append({"plugin": artifact.name, "load": "fail", "signal": "skipped",
                                    "automation": "skipped", "state": "skipped", "editor": "skipped",
                                    "editor_reason": "unknown artifact",
                                    "reason": "unknown artifact; add an explicit CASES entry"})
                continue
            plugin_id, case = pair
            if args.plugin and plugin_id not in args.plugin:
                continue
            seen.add(plugin_id)
            results.append(check_plugin(plugin_id, case, runner, directory / case.artifact,
                                        ini.parent, media, args.editor))
        for missing in set(args.plugin or ()) - seen:
            results.append({"plugin": missing, "load": "fail", "signal": "skipped",
                            "automation": "skipped", "state": "skipped", "editor": "skipped",
                            "editor_reason": "artifact absent", "reason": "artifact absent"})
    except (CheckError, OSError, KeyboardInterrupt) as error:
        fatal = str(error) or "interrupted"
        structured = error if isinstance(error, CheckError) else CheckError(fatal)
        fatal_error = structured.structured(str(directory))
    finally:
        after = snapshot(user_config)
        write_json(directory / "suite.config-after.json", after)
        changed = changes(before, after)
        if changed:
            fatal = "USER CONFIG CHANGED: " + ", ".join(changed)
            fatal_error = ConfigChanged(fatal).structured(str(user_config))
        report = {"schema": "supple.reaper-check/1", "build_dir": str(args.build_dir.resolve()),
                  "artifacts": [{"path": str(p), "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                                for p in inventory],
                  "version": next((r.get("metadata", {}).get("version") for r in results
                                   if r.get("metadata", {}).get("version")), "unavailable"),
                  "commands": runner.commands if runner else [], "results": results,
                  "editor_requested": args.editor,
                  "fatal": fatal, "error": fatal_error, "user_config_unchanged": not changed}
        write_json(directory / "report.json", report)
        print("\nREAPER " + report["version"])
        print("plugin | load | signal | automation | state | editor | reason")
        for row in results:
            columns = ("plugin", "load", "signal", "automation", "state", "editor", "reason")
            print(" | ".join(str(row[k]) for k in columns))
        if fatal:
            print("FAIL: " + fatal, file=sys.stderr)
        print("Report: " + str(directory / "report.json"))
    return int(bool(fatal) or not results or any("fail" == r[k] for r in results
                                               for k in ("load", "signal", "automation", "state", "editor")))


if __name__ == "__main__":
    def interrupt(signum, frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupt)
    sys.exit(main())
