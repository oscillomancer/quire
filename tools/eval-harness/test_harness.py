import copy
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import HERE, ROOT, Mcp, capture, evaluation_profile, load_tasks, read_json, sandbox_command, setup, slug, validate_task, write_json
from relay import Recorder, invocations
from runners import default_model, dry, isolated_config, preamble
from run import render_patch, score_final
from scoring import notation_score, transcript_metrics


class Tasks(unittest.TestCase):
    def test_catalogue(self):
        self.assertEqual([len(load_tasks(s)) for s in ("synth-descriptive", "synth-reference", "notation")], [10, 10, 5])
        refs = load_tasks("synth-reference")
        self.assertEqual({r["tier"] for r in refs}, {1, 2, 3})
        for task in refs:
            if task["tier"] < 3:
                self.assertIn(len(task["changed_modules"]), range(1, 4))

    def test_invalid_task_fields(self):
        template = load_tasks("synth-descriptive")[0]
        for key, value in (("schema", "v0"), ("id", "../escape"), ("suite", "unknown"),
                           ("recipe_index", 0), ("limits", {"tool_calls": 0, "minutes": 1}),
                           ("score", {"profile": {"schema": "supple.analysis-profile/1.0", "targets": {}}})):
            with self.subTest(key=key), self.assertRaises(ValueError):
                task = copy.deepcopy(template)
                task[key] = value
                validate_task(task)

    def test_selection(self):
        self.assertEqual(load_tasks("synth-descriptive", ["sub-bass"])[0]["id"], "synth.sub-bass")
        with self.assertRaises(ValueError):
            load_tasks("notation", ["missing"])

    def test_hidden_patch_not_in_prompt(self):
        task = load_tasks("synth-reference", ["pluck-bass"])[0]
        task["hidden_patch"]["test_private_marker"] = "NEVER-EXPOSE-THIS"
        self.assertNotIn("NEVER-EXPOSE-THIS", preamble(task, "/render/reference.wav"))
        self.assertNotIn("hidden_patch", preamble(task))


class Scoring(unittest.TestCase):
    def midi(self):
        return {"ppq": 960, "length_ticks": 7680, "meter": "4/4", "bar_meters": ["4/4", "4/4"],
                "notes": [{"pitch": 60}, {"pitch": 64}, {"pitch": 67}, {"pitch": 72}]}

    def test_notation_success_and_every_failure(self):
        target = {"bars": 2, "meter": "4/4", "pitch_range": [60, 72], "notes_per_bar": [2, 2]}
        attempts = [{"compiled": True}]
        self.assertTrue(notation_score(self.midi(), target, attempts)["pass"])
        cases = [("bars", {"length_ticks": 3840}), ("meter", {"bar_meters": ["4/4", "3/4"]}),
                 ("pitch_range", {"notes": [{"pitch": 59}, {"pitch": 64}, {"pitch": 67}, {"pitch": 72}]}),
                 ("density", {"notes": [{"pitch": 60}]})]
        for key, change in cases:
            with self.subTest(key=key):
                result = notation_score(dict(self.midi(), **change), target, attempts)
                self.assertFalse(result["checks"][key])
                self.assertFalse(result["pass"])
        result = notation_score(self.midi(), target, [{"compiled": False}, {"compiled": True}])
        self.assertFalse(result["checks"]["first_attempt"])
        self.assertFalse(notation_score(dict(self.midi(), notes=[]), target, attempts)["pass"])

    def test_transcript_batches_errors_and_last_phrase(self):
        entries = [{"name": "exec", "arguments": {"batch": {"steps": [
            {"command": "patch.merge", "args": {}}, {"command": "render", "args": {"notation": "good"}}]}},
                    "response": {"result": {"content": [{"type": "text", "text": "{}"}]}}},
                   {"name": "render", "arguments": {"notation": "bad"}, "response": {"result": {
                       "isError": True, "content": [{"type": "text", "text": '{"code":"notation.bar"}'}]}}}]
        result = transcript_metrics(entries)
        self.assertEqual((result["tool_calls"], result["errors"], result["batches"], result["patch_merges"]), (2, 1, 1, 1))
        self.assertEqual(result["notation_attempts"][-1]["notation"], "bad")
        self.assertFalse(result["notation_attempts"][-1]["compiled"])
        self.assertEqual(invocations("exec", {"command": "param.set", "args": {}}), [("param.set", {})])


class Isolation(unittest.TestCase):
    def test_dedicated_profile_without_default_fallback(self):
        self.assertEqual(evaluation_profile(), Path.home() / ".hermes/profiles" / (slug() + "-eval"))
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as directory:
            with self.assertRaises(FileNotFoundError):
                default_model(Path(directory) / "missing/config.yaml")

    def test_default_identity_excludes_secrets_and_other_config(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as directory:
            path = Path(directory) / "source.yaml"
            path.write_text('model:\n  default: cheap/model\n  provider: openrouter\n  api_key: private\nmcp_servers:\n  unrelated: bad\n')
            self.assertEqual(default_model(path), {"default": "cheap/model", "provider": "openrouter"})

    def test_isolation_reference_and_tool_restriction(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as directory:
            root = Path(directory)
            credential = root / "external.env"
            credential.write_text("OPENROUTER_API_KEY=FAKE-UNIT-TEST\n")
            task = load_tasks("synth-descriptive", ["sub-bass"])[0]
            with mock.patch.dict(os.environ, {"HERMES_INFERENCE_MODEL": "must-not-inherit"}):
                home, env = isolated_config(task, root / "run", 42,
                                             {"default": "test", "provider": "test"}, credential,
                                             binary=sys.executable)
            config = read_json(home / "config.yaml")
            self.assertEqual(set(config["mcp_servers"]), {"sandbox"})
            self.assertEqual(config["platform_toolsets"]["cli"], ["sandbox"])
            self.assertTrue((home / ".env").is_symlink())
            self.assertNotIn("FAKE-UNIT-TEST", (home / "config.yaml").read_text())
            self.assertNotIn("HERMES_INFERENCE_MODEL", env)
            self.assertEqual(credential.read_text(), "OPENROUTER_API_KEY=FAKE-UNIT-TEST\n")
            self.assertNotIn("hidden_patch", (root / "run/relay.json").read_text())


@unittest.skipUnless(os.environ.get("SUPPLE_EVAL_INTEGRATION") == "1", "opt-in real sandbox checks")
class Integration(unittest.TestCase):
    def test_dry_all_scorers_authoritative_capture_and_reference_gain(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as directory:
            root = Path(directory)
            for suite, task_id in (("synth-descriptive", "sub-bass"), ("synth-reference", "sub-bass"), ("notation", "waltz")):
                task = load_tasks(suite, [task_id])[0]
                reference = None
                if suite == "synth-reference":
                    reference, _ = render_patch(task, task["hidden_patch"], root / "hidden", 42)
                run_dir = root / suite
                dry(task, run_dir, 42)
                scored = score_final(task, run_dir, 42, reference=reference)
                self.assertTrue(scored["score"]["pass"], scored)
                self.assertTrue(scored["valid_patch"])
                if reference:
                    self.assertAlmostEqual(scored["score"]["loss"], 0, places=10)
                    wav = bytearray(reference.read_bytes())
                    offset = 12
                    while wav[offset:offset + 4] != b"data":
                        size = struct.unpack_from("<I", wav, offset + 4)[0]
                        offset += 8 + size + size % 2
                    size = struct.unpack_from("<I", wav, offset + 4)[0]
                    values = struct.unpack_from(f"<{size // 4}f", wav, offset + 8)
                    struct.pack_into(f"<{len(values)}f", wav, offset + 8, *(v * .5 for v in values))
                    quiet = root / "quieter.wav"
                    quiet.write_bytes(wav)
                    args = {"reference": str(reference), "test": str(quiet),
                            "reference_note_off_s": 2, "test_note_off_s": 2}
                    result = subprocess.run([sandbox_command(root)[0], "eval-reference", json.dumps(args)],
                                            capture_output=True, text=True, check=True)
                    self.assertLess(json.loads(result.stdout)["loss"], 1e-6)
                if suite == "synth-descriptive":
                    task["score"]["profile"]["targets"]["spectral.centroid_hz.median"] = {"min": 10000, "max": 11000}
                    self.assertFalse(score_final(task, run_dir, 42)["score"]["pass"])

    def test_limits_capture_batch_and_invalid_final(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as directory:
            root = Path(directory)
            task = load_tasks("synth-descriptive", ["sub-bass"])[0]
            with Mcp(sandbox_command(root), root) as client:
                setup(client, task, 42)
                recorder = Recorder(client, root, 1)
                message = {"jsonrpc": "2.0", "id": 100, "method": "tools/call", "params": {
                    "name": "batch", "arguments": {"steps": [
                        {"command": "patch.merge", "args": {"instance": "in_1", "patch": {"seed": 91}}},
                        {"command": "param.set", "args": {"instance": "in_1", "values": {"output.level": -9}}}]}}}
                self.assertFalse(recorder.handle(message)["result"].get("isError", False))
                final = read_json(root / "final.json")
                self.assertEqual(final["patches"]["in_1"]["seed"], 91)
                message["id"] = 101
                self.assertTrue(recorder.handle(message)["result"]["isError"])
                recorder.close()
                client.call("plugin.remove", {"instance": "in_1"})
                capture(client, root / "final.json")
            with self.assertRaisesRegex(ValueError, "Final chain"):
                score_final(task, root, 42)


if __name__ == "__main__":
    unittest.main()
