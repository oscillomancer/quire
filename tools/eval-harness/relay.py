# SPDX-License-Identifier: GPL-3.0-or-later
"""MCP relay: expose sandbox tools and capture authoritative state, never prose."""

import argparse
import json
from pathlib import Path
import signal
import sys

from common import (Mcp, PluginProvenanceError, capture, check_plugin_provenance, plugin_dir,
                    read_json, setup, tool_data, write_json)


def invocations(name, args):
    """Flatten explicit batches for efficiency/notation accounting."""
    if name in ("exec", "query"):
        if "batch" in args:
            return invocations("batch", args["batch"])
        return invocations(args.get("command", ""), args.get("args", {}))
    if name == "batch":
        return [entry for step in args.get("steps", [])
                for entry in invocations(step.get("command", ""), step.get("args", {}))]
    return [(name.split("@", 1)[0].replace("_", "."), args)]


class Recorder:
    def __init__(self, client, workdir, limit):
        self.client = client
        self.workdir = Path(workdir)
        self.limit = limit
        self.calls = 0
        self.log = (self.workdir / "tools.jsonl").open("a")

    def handle(self, message):
        if message.get("method") != "tools/call":
            return self.client.request(message)
        self.calls += 1
        params = message["params"]
        if self.calls > self.limit:
            result = {"isError": True, "content": [{"type": "text", "text": json.dumps({
                "code": "eval.tool_limit", "message": "Tool-call budget exhausted",
                "hint": "Finish with the patch already in the sandbox"})}]}
            response = {"jsonrpc": "2.0", "id": message["id"], "result": result}
        else:
            response = self.client.request(message)
        self.log.write(json.dumps({"call": self.calls, "name": params["name"],
                                   "arguments": params.get("arguments", {}),
                                   "response": response}) + "\n")
        self.log.flush()
        # Includes edits inside script.run, batches, preset loads, and completed jobs.
        capture(self.client, self.workdir / "final.json")
        return response

    def close(self):
        self.log.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, required=True)
    args = parser.parse_args()
    config = read_json(args.config)
    workdir = Path(config["workdir"])
    client = Mcp(config["sandbox_command"], workdir, own_group=False)
    recorder = Recorder(client, workdir, config["limit"])
    initialized = False

    def interrupted(*_):
        raise SystemExit(130)

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        for line in sys.stdin:
            message = json.loads(line)
            if message.get("method") == "initialize" and not initialized:
                response = client.request(message)
                client.request({"jsonrpc": "2.0", "method": "notifications/initialized"})
                setup(client, config["task"], config["seed"])
                if config.get("starting_patch"):
                    patch = dict(config["starting_patch"], seed=config["seed"])
                    client.call("patch.set", {"instance": "in_1", "patch": patch})
                capture(client, workdir / "final.json")
                write_json(workdir / "exposed-tools.json", client.rpc("tools/list"))
                # Record which plugin files this attempt runs, and refuse to go on
                # when one of them is not the build's: a stale copy installed in
                # ~/.clap shadowed the built Orrery for two evaluation campaigns
                # (2026-09-30..10-01), so their numbers measured another plugin.
                expect = plugin_dir(config["sandbox_command"])
                try:
                    provenance = check_plugin_provenance(client, expect, what="the attempt")
                except PluginProvenanceError as error:
                    write_json(workdir / "plugins.json",
                               {"expected": str(expect), "error": str(error)})
                    print(f"eval.plugin_provenance: {error}", file=sys.stderr, flush=True)
                    raise SystemExit(3) from error
                write_json(workdir / "plugins.json",
                           {"expected": str(expect), "plugins": provenance})
                initialized = True
            else:
                response = recorder.handle(message)
            if response is not None:
                print(json.dumps(response), flush=True)
            if recorder.calls > recorder.limit:
                break
    finally:
        if initialized and client.process.poll() is None:
            try:
                capture(client, workdir / "final.json")
            except (RuntimeError, TimeoutError, ValueError, BrokenPipeError):
                pass  # Every completed call already has a durable capture.
        recorder.close()
        client.close()


if __name__ == "__main__":
    main()
