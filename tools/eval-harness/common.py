# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline evaluation utilities; Python standard library only."""

import hashlib
import json
import math
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def read_json(path):
    return json.loads(Path(path).read_text())


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n")
    temporary.replace(path)


def slug():
    text = (ROOT / "cmake/Product.cmake").read_text()
    return re.search(r'set\(PRODUCT_SLUG\s+"([a-z0-9-]+)"\)', text)[1]


def evaluation_profile():
    return Path.home() / ".hermes/profiles" / (slug() + "-eval")


def skill_hash():
    digest = hashlib.sha256()
    for path in sorted((ROOT / "skill").rglob("*")):
        if path.is_file():
            digest.update(str(path.relative_to(ROOT)).encode() + b"\0" + path.read_bytes())
    return digest.hexdigest()


def sandbox_command(workdir, binary=None):
    binary = binary or ROOT / "build/dev/host/sandbox" / (slug() + "-sandbox")
    if not Path(binary).is_file():
        raise ValueError("Build the dev sandbox first, or supply --sandbox.")
    return [str(Path(binary).resolve()), "--mcp", "--record-midi", "--workdir", str(workdir),
            "--plugin-path", str(ROOT / "build/dev/plugins/clap")]


class PluginProvenanceError(ValueError):
    """A session loaded a plugin that is not the build under test."""


def plugin_dir(command=None):
    """The plugin directory the harness searches (the command's --plugin-path)."""
    command = list(command or sandbox_command("/tmp"))
    for i, arg in enumerate(command):
        if arg == "--plugin-path" and i + 1 < len(command):
            return Path(command[i + 1]).resolve()
    return (ROOT / "build/dev/plugins/clap").resolve()


def load_provenance(client):
    """{instance: {plugin, path, version, shadowed}} for the plugins a session loaded.

    `plugin.list` reports each plugin's file and flags the copies it shadowed,
    and a session resolves an id to the first entry on the search path: the
    first entry per id is the one in use.
    """
    session = client.call("session.get")
    chosen, shadowed = {}, {}
    for entry in client.call("plugin.list", {})["plugins"]:
        chosen.setdefault(entry["id"], entry)
        if entry.get("shadowed"):
            shadowed.setdefault(entry["id"], []).append(entry.get("path"))
    out = {}
    for instance in session["chain"]:
        plugin = session["instances"][instance]["plugin"]
        entry = chosen.get(plugin, {})
        out[instance] = {"plugin": plugin, "path": entry.get("path"),
                         "version": entry.get("version"),
                         "shadowed": shadowed.get(plugin, [])}
    return out


def check_plugin_provenance(client, expect_dir=None, what="the session"):
    """Every plugin a session loaded must come from the build under test.

    The sandbox opens the first file on its search path for an id, so a stale
    copy installed elsewhere (a release in ~/.clap) used to shadow the built
    one: an eval attempt or the optimizer could run a different plugin than the
    scorer, which is silent and worthless to measure. Raises, loudly, instead.
    """
    expect = Path(expect_dir or plugin_dir()).resolve()
    provenance = load_provenance(client)
    bad = {}
    for instance, entry in provenance.items():
        path = entry.get("path")
        candidates = []
        if path:
            given = Path(path)
            candidates = [given.resolve() if given.is_absolute() else (base / given).resolve()
                          for base in (Path.cwd(), ROOT)]
        if not any(c.is_relative_to(expect) for c in candidates):
            bad[instance] = entry
    if bad:
        raise PluginProvenanceError(
            f"{what} loaded plugins outside the build ({expect}): {json.dumps(bad)}; "
            "a stale copy is shadowing it (plugin.list flags it as shadowed) — remove it or "
            "fix the search path before measuring")
    return provenance


def stop_group(process):
    # Kill descendants too, even if their parent has already exited.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


class Mcp:
    """A bounded, serial JSON-lines MCP client, usable as a transparent relay."""

    def __init__(self, command, workdir, env=None, own_group=True):
        self.workdir = Path(workdir)
        self.workdir.mkdir(parents=True, exist_ok=True)
        self.stderr = (self.workdir / "sandbox.stderr").open("a")
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.stderr, env=env, start_new_session=own_group)
        self.own_group = own_group
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.buffer = b""
        self.serial = 0

    def receive(self, timeout=90):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            if not self.selector.select(max(0, deadline - time.monotonic())):
                raise TimeoutError("Sandbox MCP response timed out")
            block = os.read(self.process.stdout.fileno(), 65536)
            if not block:
                raise RuntimeError("Sandbox MCP exited; see sandbox.stderr")
            self.buffer += block
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def request(self, message, timeout=90):
        self.process.stdin.write((json.dumps(message) + "\n").encode())
        self.process.stdin.flush()
        if "id" not in message:
            return None
        while True:
            response = self.receive(timeout)
            if response.get("id") == message["id"]:
                return response

    def rpc(self, method, params=None):
        self.serial += 1
        response = self.request({"jsonrpc": "2.0", "id": f"eval-{self.serial}",
                                 "method": method, "params": params or {}})
        if "error" in response:
            raise ValueError(response["error"])
        return response["result"]

    def initialize(self):
        result = self.rpc("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                          "clientInfo": {"name": "supple-eval", "version": "1"}})
        self.request({"jsonrpc": "2.0", "method": "notifications/initialized"})
        return result

    def call_raw(self, name, arguments=None):
        return self.rpc("tools/call", {"name": name, "arguments": arguments or {}})

    def call(self, name, arguments=None):
        # Jobs and registry queries are long-tail commands, accessed via exec.
        result = self.call_raw("exec", {"command": name, "args": arguments or {}})
        data = tool_data(result)
        if result.get("isError"):
            raise ValueError(data)
        return data

    def close(self):
        try:
            self.process.stdin.close()
            self.process.wait(timeout=3)
        except (BrokenPipeError, subprocess.TimeoutExpired):
            if self.own_group:
                stop_group(self.process)
            else:
                self.process.kill()
                self.process.wait()
        finally:
            self.selector.close()
            self.process.stdout.close()
            self.stderr.close()

    def __enter__(self):
        self.initialize()
        return self

    def __exit__(self, *_):
        self.close()


def tool_data(result):
    if "structuredContent" in result:
        return result["structuredContent"]
    text = "\n".join(item.get("text", "") for item in result.get("content", [])
                     if item.get("type") == "text")
    return json.loads(text) if text else {}


def capture(client, path):
    session = client.call("session.get")
    patches = {}
    for instance in session["chain"]:
        if session["instances"][instance]["semantic"]:
            patches[instance] = client.call("patch.get", {"instance": instance, "detail": "full"})["patch"]
    result = {"schema": "supple.eval-final/1.0", "session": session, "patches": patches}
    write_json(path, result)
    return result


def setup(client, task, seed):
    client.call("session.set", task["settings"])
    for step in task["setup"]:
        client.call(step["command"], step.get("args", {}))
    client.call("patch.merge", {"instance": "in_1", "patch": {"seed": seed}})


def recipe_patch(task):
    text = (ROOT / "skill/guides/sound-design.md").read_text()
    recipes = [json.loads(block) for block in re.findall(r"```json\n(.*?)\n```", text, re.S)]
    return recipes[task["recipe_index"]]


def validate_task(task):
    def require(condition, message):
        if not condition:
            raise ValueError(f"{task.get('id', '<task>')}: {message}")
    require(task.get("schema") == "supple.eval-task/1.0", "wrong schema")
    require(task.get("suite") in ("synth-descriptive", "synth-reference", "notation"), "wrong suite")
    require(bool(re.fullmatch(r"[a-z][a-z0-9.-]+", task.get("id", ""))), "invalid id")
    for field in ("prompt", "listening", "phrase"):
        require(isinstance(task.get(field), str) and bool(task[field]), f"missing {field}")
    require(isinstance(task.get("setup"), list) and bool(task["setup"]), "missing setup")
    require(task["setup"][0]["args"]["plugin"] == "com.suppleaudio.orrery", "expected Orrery")
    limits = task.get("limits", {})
    require(type(limits.get("tool_calls")) is int and 1 <= limits["tool_calls"] <= 200, "bad call limit")
    require(isinstance(limits.get("minutes"), (int, float)) and 0 < limits["minutes"] <= 30, "bad time limit")
    render = task.get("render", {})
    require(0 <= render.get("tail_s", -1) <= 5, "bad tail limit")
    require(type(task.get("recipe_index")) is int and 1 <= task["recipe_index"] <= 10, "bad recipe index")
    require(isinstance(task.get("settings"), dict), "missing settings")
    score = task.get("score", {})
    if task["suite"] == "synth-descriptive":
        profile = score.get("profile", {})
        require(profile.get("schema") == "supple.analysis-profile/1.0", "bad profile schema")
        require(bool(profile.get("targets")), "empty profile")
        for key, target in profile["targets"].items():
            require(isinstance(key, str) and isinstance(target, dict), "bad target")
            require(all(type(v) in (int, float) and math.isfinite(v) for v in target.values()), "non-finite target")
            require(("min" in target and "max" in target and target["min"] < target["max"])
                    or ("target" in target and target.get("tolerance", 0) > 0), "invalid range")
            require(target.get("weight", 1) > 0, "invalid weight")
    elif task["suite"] == "synth-reference":
        require(task.get("tier") in (1, 2, 3), "bad tier")
        require(isinstance(task.get("hidden_patch"), dict), "missing hidden patch")
        require(score.get("max_loss", 0) > 0, "invalid reference threshold")
        require(task["tier"] == 3 or isinstance(task.get("starting_patch"), dict), "missing starting patch")
        require(task["tier"] == 3 or 1 <= len(task.get("changed_modules", [])) <= 3, "invalid module count")
    else:
        checks = score.get("structure", {})
        require(checks.get("bars", 0) > 0 and re.fullmatch(r"[0-9]+/[0-9]+", checks.get("meter", "")), "bad structure")
        require(len(checks.get("pitch_range", [])) == 2 and len(checks.get("notes_per_bar", [])) == 2, "bad bounds")
    return task


def load_tasks(suite, selected=None):
    tasks = [validate_task(read_json(path)) for path in sorted((HERE / "tasks" / suite).glob("*.json"))]
    if selected:
        wanted = set(selected)
        tasks = [task for task in tasks if task["id"] in wanted or task["id"].split(".", 1)[1] in wanted]
        if len(tasks) != len(wanted):
            raise ValueError("Unknown or duplicate task selector")
    if not tasks:
        raise ValueError("No tasks selected")
    return tasks
