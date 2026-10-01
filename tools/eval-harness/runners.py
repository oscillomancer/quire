"""Scripted and isolated Hermes runners."""

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import threading
import time

from common import HERE, ROOT, Mcp, capture, evaluation_profile, recipe_patch, sandbox_command, setup, stop_group, write_json
from relay import Recorder


def preamble(task, reference=None, seed=42, blind=False):
    files = ["SKILL.md", "guides/sandbox-loop.md", "guides/analysis.md", "guides/notation.md"]
    if task["suite"] != "notation":
        files.append("guides/sound-design.md")
    skill = "\n\n".join((ROOT / "skill" / name).read_text() for name in files)
    prompt = ("You are being evaluated using only the sandbox MCP tools. The following is the "
              "bundled product skill (guides are supplied here because file tools are disabled).\n\n"
              + skill + "\n\nTASK\n" + task["prompt"] +
              "\nOrrery is already loaded as in_1. Leave your final patch in that instance. "
              "Do not remove it or load other plugins. You may inspect its full schema with "
              "plugin_describe. Finish after measuring the result; no listening approval is needed.\n")
    if task["suite"] == "notation":
        prompt += "Submit your phrase with the render tool's notation argument. The last notation "\
                  "submitted for rendering is the final answer; prose is not scored.\n"
    else:
        prompt += "Render this fixed evaluation phrase (also used by the scorer):\n" + task["phrase"] + "\n"
    prompt += "Fixed render options: " + json.dumps(task["render"]) + "\n"
    if task["score"].get("profile") and not blind:
        prompt += "Descriptor targets: " + json.dumps(task["score"]["profile"]) + "\n"
    if reference:
        prompt += ("Match the reference audio at " + str(reference) + ". Use analyze/compare or the "
                   "optimizer to measure it. The hidden patch is unavailable. You are scored by "
                   "compare's loss.total against that reference (lower is better); it passes at "
                   f"or below the task's max_loss of {task['score']['max_loss']}.\n")
    prompt += f"Budget: {task['limits']['tool_calls']} MCP calls, {task['limits']['minutes']} minutes.\n"
    prompt += f"Use patch seed {seed}; the scorer restores this same seed for reproducibility.\n"
    return prompt


def dry(task, directory, seed, binary=None):
    directory = Path(directory)
    with Mcp(sandbox_command(directory, binary), directory) as client:
        setup(client, task, seed)
        recorder = Recorder(client, directory, task["limits"]["tool_calls"])
        serial = 0

        def call(name, arguments):
            nonlocal serial
            serial += 1
            return recorder.handle({"jsonrpc": "2.0", "id": serial, "method": "tools/call",
                                    "params": {"name": name, "arguments": arguments}})

        patch = task.get("hidden_patch", recipe_patch(task))
        call("patch_set", {"instance": "in_1", "patch": dict(patch, seed=seed)})
        call("patch_merge", {"instance": "in_1", "patch": {"seed": seed}})
        call("render", dict(task["render"], notation=task["phrase"]))
        capture(client, directory / "final.json")
        recorder.close()
    write_json(directory / "usage.json", {"model": "scripted", "provider": "none", "total_tokens": 0,
                                           "estimated_cost_usd": 0, "completed": True})
    (directory / "transcript.jsonl").write_text((directory / "tools.jsonl").read_text())
    return {"exit_code": 0, "timed_out": False, "model": "scripted", "provider": "none"}


def default_model(source):
    """Read only the nonsecret model identity from JSON or simple YAML config.

    Hermes owns YAML parsing. No YAML dependency is introduced into the harness;
    unusual YAML requires an explicit JSON model identity file via --model-config.
    """
    text = Path(source).read_text()
    keys = {"default", "provider", "base_url", "api_mode", "context_length"}
    if text.lstrip().startswith("{"):
        model = json.loads(text)["model"]
        result = {key: value for key, value in model.items() if key in keys}
    else:
        block = re.search(r"(?m)^model:\s*\n((?:[ \t]+[^\n]*\n|\n)*)", text)
        if not block:
            raise ValueError("Hermes model config needs a simple model mapping, or --model-config JSON")
        result = {}
        for line in block[1].splitlines():
            match = re.fullmatch(r"  ([a-z_]+):\s*(.*?)\s*", line)
            if match and match[1] in keys:
                value = match[2]
                if value.startswith('"'):
                    value = json.loads(value)
                elif value.startswith("'") and value.endswith("'"):
                    value = value[1:-1].replace("''", "'")
                elif any(c in value for c in "{}[]&*#!"):
                    raise ValueError("Unsupported model YAML; supply --model-config JSON")
                elif match[1] == "context_length":
                    value = int(value)
                result[match[1]] = value
    if not result.get("default") or not result.get("provider"):
        raise ValueError("Hermes default model and provider must be configured")
    return result


def isolated_config(task, directory, seed, model, credentials, binary=None, starting_patch=None):
    directory = Path(directory).resolve()
    home = directory / "hermes"
    home.mkdir(parents=True, exist_ok=False, mode=0o700)
    # Credentials are referenced, never serialized into configs/reports or copied.
    if credentials:
        (home / ".env").symlink_to(Path(credentials).expanduser().resolve(strict=True))
    relay_config = {"workdir": str(directory), "sandbox_command": sandbox_command(directory, binary),
                    "limit": task["limits"]["tool_calls"], "seed": seed,
                    "task": {key: task[key] for key in ("settings", "setup")}}
    if starting_patch:
        relay_config["starting_patch"] = starting_patch
    write_json(directory / "relay.json", relay_config)
    config = {
        "model": model,
        "agent": {"max_turns": task["limits"]["tool_calls"] + 2, "verbose": False},
        "platform_toolsets": {"cli": ["sandbox"]},
        "memory": {"memory_enabled": False, "user_profile_enabled": False},
        "compression": {"enabled": False},
        "session": {"auto_title": False},
        "mcp_servers": {"sandbox": {"command": sys.executable,
                                      "args": [str(HERE / "relay.py"), "--config", str(directory / "relay.json")],
                                      "timeout": 120}},
    }
    write_json(home / "config.yaml", config)  # JSON is a YAML subset.
    env = {key: value for key, value in os.environ.items() if not key.startswith("HERMES_")}
    env.update(HERMES_HOME=str(home), HERMES_DISABLE_LAZY_INSTALLS="1", PYTHONDONTWRITEBYTECODE="1",
               HERMES_IGNORE_RULES="1")
    return home, env


# Live attempts' process groups (each Hermes runs in its own session), so an
# interrupted run can stop them instead of orphaning them.
ACTIVE = set()
ACTIVE_LOCK = threading.Lock()


def stop_all():
    with ACTIVE_LOCK:
        live = list(ACTIVE)
    for process in live:
        stop_group(process)


def hermes_command(options):
    return ([options.hermes_python, "-I", "-B", "-m", "hermes_cli.main"]
            if options.hermes_python else [options.hermes_bin])


def preflight(options, directory):
    """Fail before any model call if the CLI cannot start under an isolated HERMES_HOME.

    `--version` succeeds even when the dependencies do not load; `config path`
    imports the configuration stack that every real attempt needs.
    """
    home = Path(directory).resolve() / "preflight-home"
    home.mkdir(parents=True, mode=0o700)
    env = {key: value for key, value in os.environ.items() if not key.startswith("HERMES_")}
    env.update(HERMES_HOME=str(home), HERMES_DISABLE_LAZY_INSTALLS="1", PYTHONDONTWRITEBYTECODE="1")
    done = subprocess.run(hermes_command(options) + ["config", "path"], env=env, cwd=directory,
                          capture_output=True, text=True, timeout=60)
    if done.returncode:
        last = ((done.stderr or done.stdout).strip().splitlines() or ["no output"])[-1]
        raise RuntimeError(f"Hermes cannot start under an isolated HERMES_HOME ({last}); a managed "
                           "install's launcher finds its dependencies through the default home, so "
                           "pass --hermes-python with the install's existing venv interpreter")


def hermes(task, directory, seed, options, reference=None):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    source = Path(options.model_config or evaluation_profile() / "config.yaml")
    model = default_model(source)
    _, env = isolated_config(task, directory, seed, model, options.credentials, options.sandbox,
                             task.get("starting_patch"))
    command = hermes_command(options)
    version = subprocess.run(command + ["--version"], env=env, cwd=directory,
                             capture_output=True, text=True, timeout=30, check=True).stdout.strip()
    prompt = preamble(task, reference, seed, getattr(options, "blind", False))
    (directory / "prompt.txt").write_text(prompt)
    started = time.monotonic()
    timed_out = False
    with (directory / "final-message.txt").open("w") as stdout, (directory / "hermes.stderr").open("w") as stderr:
        process = subprocess.Popen(command + ["-z", prompt, "--usage-file", str(directory / "usage.json"),
                                               "--toolsets", "sandbox", "--ignore-rules"],
                                   env=env, cwd=directory, stdout=stdout, stderr=stderr, start_new_session=True)
        with ACTIVE_LOCK:
            ACTIVE.add(process)
        try:
            process.wait(timeout=task["limits"]["minutes"] * 60)
        except subprocess.TimeoutExpired:
            timed_out = True
        finally:
            stop_group(process)
            with ACTIVE_LOCK:
                ACTIVE.discard(process)
    export = subprocess.run(command + ["sessions", "export", str(directory / "transcript.jsonl"),
                                       "--format", "jsonl", "--redact"],
                            env=env, cwd=directory, capture_output=True, text=True, timeout=30)
    (directory / "export.log").write_text(export.stdout + export.stderr)
    if export.returncode:
        raise RuntimeError("Hermes transcript export failed; see export.log")
    return {"exit_code": process.returncode, "timed_out": timed_out, "model": model["default"],
            "provider": model["provider"], "hermes_version": version,
            "wall_seconds": time.monotonic() - started, "skill_loading": "task-preamble",
            "hermes_profile": source.parent.name, "model_config": str(source),
            "credentials_reference": str(Path(options.credentials).expanduser().resolve())}
