# SPDX-License-Identifier: GPL-3.0-or-later
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
from common import HERE, ROOT, SYNTH, Mcp, PluginProvenanceError, capture, check_plugin_provenance, evaluation_profile, load_tasks, read_json, sandbox_command, setup, slug, validate_task, write_json
from relay import Recorder, invocations
from runners import default_model, dry, isolated_config, preamble, preflight
from run import render_patch, score_final, unpublishable
from scoring import notation_score, transcript_metrics
from process_tree import spawn_group, stop_group
from pipe_ready import wait_pipe


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

    def test_blind_hides_descriptor_targets_only(self):
        task = load_tasks("synth-descriptive", ["pad"])[0]
        self.assertIn("Descriptor targets", preamble(task))
        blind = preamble(task, blind=True)
        self.assertNotIn("Descriptor targets", blind)
        self.assertNotIn("supple.analysis-profile", blind)
        self.assertIn(task["prompt"], blind)

    def test_reference_preamble_states_the_scoring_rule(self):
        task = load_tasks("synth-reference", ["sub-bass"])[0]
        text = preamble(task, "/render/reference.wav")
        # The scored loss is named (the guide mentions loss.total too, so match
        # the rule's own wording).
        self.assertIn("You are scored by compare's loss.total", text)
        self.assertIn(str(task["score"]["max_loss"]), text)
        self.assertNotIn("You are scored by", preamble(task))


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

    def test_explicit_command_versions_count_as_notation_attempts(self):
        entries = [{"name": "exec", "arguments": {"command": "render@1", "args": {"notation": "good"}},
                    "response": {"result": {"structuredContent": {"render": "rd_1"}}}},
                   {"name": "batch", "arguments": {"steps": [
                       {"command": "render@1", "args": {"notation": "bad"}}]},
                    "response": {"result": {"isError": True, "structuredContent": {"code": "notation.bar"}}}}]
        attempts = transcript_metrics(entries)["notation_attempts"]
        self.assertEqual([a["notation"] for a in attempts], ["good", "bad"])
        self.assertTrue(attempts[0]["compiled"])
        self.assertFalse(attempts[1]["compiled"])


class Isolation(unittest.TestCase):
    def test_dedicated_profile_without_default_fallback(self):
        self.assertEqual(evaluation_profile(), Path.home() / ".hermes/profiles" / (slug() + "-eval"))
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as directory:
            with self.assertRaises(FileNotFoundError):
                default_model(Path(directory) / "missing/config.yaml")

    def test_default_identity_excludes_secrets_and_other_config(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as directory:
            path = Path(directory) / "source.yaml"
            path.write_text('model:\n  default: cheap/model\n  provider: openrouter\n  api_key: private\nmcp_servers:\n  unrelated: bad\n', encoding="utf-8")
            self.assertEqual(default_model(path), {"default": "cheap/model", "provider": "openrouter"})

    def test_isolation_reference_and_tool_restriction(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as directory:
            root = Path(directory)
            credential = root / "external.env"
            credential.write_text("OPENROUTER_API_KEY=FAKE-UNIT-TEST\n", encoding="utf-8")
            task = load_tasks("synth-descriptive", ["sub-bass"])[0]
            with mock.patch.dict(os.environ, {"HERMES_INFERENCE_MODEL": "must-not-inherit"}):
                home, env = isolated_config(task, root / "run",
                                             {"default": "test", "provider": "test"}, credential,
                                             binary=sys.executable)
            config = read_json(home / "config.yaml")
            self.assertEqual(set(config["mcp_servers"]), {"sandbox"})
            self.assertEqual(config["platform_toolsets"]["cli"], ["sandbox"])
            self.assertTrue((home / ".env").samefile(credential))
            self.assertNotIn("FAKE-UNIT-TEST", (home / "config.yaml").read_text(encoding="utf-8"))
            self.assertNotIn("HERMES_INFERENCE_MODEL", env)
            self.assertEqual(credential.read_text(encoding="utf-8"), "OPENROUTER_API_KEY=FAKE-UNIT-TEST\n")
            self.assertNotIn("hidden_patch", (root / "run/relay.json").read_text(encoding="utf-8"))


    def test_preflight_rejects_a_cli_that_cannot_load_under_an_isolated_home(self):
        with tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp) / "hermes.py"
            fake.write_text("import sys; print('No module named ruamel', file=sys.stderr); sys.exit(1)\n", encoding="utf-8")
            options = mock.Mock(hermes_python=None, hermes_bin=str(fake))
            with mock.patch("runners.hermes_command", return_value=[sys.executable, str(fake)]):
                with self.assertRaisesRegex(RuntimeError, "ruamel.*--hermes-python"):
                    preflight(options, Path(tmp) / "a")
                fake.write_text("import os; from pathlib import Path; assert Path(os.environ['HERMES_HOME']).is_dir()\n", encoding="utf-8")
                preflight(options, Path(tmp) / "b")

    def test_descendants_are_reaped_after_the_leader_exits(self):
        with tempfile.TemporaryDirectory() as directory:
            code = ("import subprocess,sys; subprocess.Popen([sys.executable, '-c', "
                    "'import time; time.sleep(60)']); print('spawned', flush=True)")
            process = spawn_group([sys.executable, "-u", "-c", code], cwd=directory,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                self.assertTrue(wait_pipe(process.stdout, 30))
                self.assertEqual(process.stdout.readline().rstrip(b"\r\n"), b"spawned")
                process.wait(timeout=30)
                stop_group(process)
                self.assertTrue(wait_pipe(process.stdout, 30))
                self.assertEqual(process.stdout.read(), b"")
            finally:
                stop_group(process)
                process.stdout.close()
                process.stderr.close()

    def test_owned_process_preserves_arguments_and_exit_status(self):
        with tempfile.TemporaryDirectory() as directory:
            arguments = ["a b", 'a"b', r"C:\path with spaces\file"]
            process = spawn_group([sys.executable, "-c",
                                   "import json,sys; print(json.dumps(sys.argv[1:])); sys.exit(7)",
                                   *arguments], cwd=directory,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                output, error = process.communicate(timeout=30)
                self.assertEqual(process.returncode, 7, error)
                self.assertEqual(json.loads(output), arguments)
            finally:
                stop_group(process)

    def test_mcp_pipe_round_trip_without_a_sandbox(self):
        with tempfile.TemporaryDirectory() as directory:
            code = "import sys; [print(line, end='', flush=True) for line in sys.stdin]"
            client = Mcp([sys.executable, '-u', '-c', code], directory)
            try:
                self.assertEqual(client.request(
                    {"jsonrpc": "2.0", "id": 4, "method": "echo", "params": {"text": "café"}}),
                    {"jsonrpc": "2.0", "id": 4, "method": "echo", "params": {"text": "café"}})
            finally:
                client.close()


class PluginProvenance(unittest.TestCase):
    """The harness must refuse to measure a plugin that is not the build's."""

    class Stub:
        def __init__(self, session, catalogue):
            self.session = session
            self.catalogue = catalogue

        def call(self, name, arguments=None):
            if name == "session.get":
                return self.session
            if name == "plugin.list":
                return self.catalogue
            raise AssertionError(name)

    def client(self, path, shadowed=None):
        entries = [{"id": SYNTH, "path": path, "version": "0.1.0"}]
        for stale in shadowed or []:
            entries.append({"id": SYNTH, "path": stale, "version": "0.1.0", "shadowed": True})
        session = {"chain": ["in_1"], "instances": {"in_1": {"plugin": SYNTH}}}
        return self.Stub(session, {"plugins": entries})

    def test_the_build_passes_and_a_stale_copy_fails(self):
        build = ROOT / "build/dev/plugins/clap"
        fresh = self.client(str(build / "Marram.clap"), [str(Path.home() / ".clap/Marram.clap")])
        provenance = check_plugin_provenance(fresh, build, what="a test session")
        self.assertEqual(provenance["in_1"]["path"], str(build / "Marram.clap"))
        self.assertEqual(provenance["in_1"]["shadowed"], [str(Path.home() / ".clap/Marram.clap")])
        # The path the sandbox prints may be relative to the harness's cwd.
        relative = check_plugin_provenance(self.client("build/dev/plugins/clap/Marram.clap"), build)
        self.assertEqual(relative["in_1"]["path"], "build/dev/plugins/clap/Marram.clap")
        for bad in (str(Path.home() / ".clap/Marram.clap"), "/tmp/Marram.clap", None):
            with self.subTest(path=bad):
                with self.assertRaises(PluginProvenanceError):
                    check_plugin_provenance(self.client(bad), build)


class Publishing(unittest.TestCase):
    def test_outages_are_not_measurements(self):
        usage = {"total_tokens": 27367}
        broken = {"usage_totals": usage, "tasks": [{"incomplete": True}, {"error": "HTTP 402"}]}
        self.assertEqual(unpublishable("hermes", broken), "every attempt was incomplete")
        silent = {"usage_totals": {"total_tokens": 0}, "tasks": [{"score": {"pass": True}}]}
        self.assertEqual(unpublishable("hermes", silent), "no attempt reached the model")
        partial = {"usage_totals": usage, "tasks": [{"incomplete": True}, {"score": {"pass": False}}]}
        self.assertIsNone(unpublishable("hermes", partial))
        self.assertIsNone(unpublishable("dry", broken))


@unittest.skipUnless(os.environ.get("SUPPLE_EVAL_INTEGRATION") == "1", "opt-in real sandbox checks")
class Integration(unittest.TestCase):
    def test_dry_all_scorers_authoritative_capture_and_reference_gain(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as directory:
            root = Path(directory)
            for suite, task_id in (("synth-descriptive", "sub-bass"), ("synth-reference", "sub-bass"), ("notation", "waltz")):
                task = load_tasks(suite, [task_id])[0]
                reference = None
                if suite == "synth-reference":
                    reference, _ = render_patch(task, task["hidden_patch"], root / "hidden")
                run_dir = root / suite
                dry(task, run_dir)
                scored = score_final(task, run_dir, reference=reference)
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
                    self.assertFalse(score_final(task, run_dir)["score"]["pass"])

    def test_compare_reports_the_same_loss_as_the_scorer(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as directory:
            root = Path(directory)
            task = load_tasks("synth-reference", ["sub-bass"])[0]
            reference, _ = render_patch(task, task["hidden_patch"], root / "hidden")
            reference_end = read_json(root / "hidden/reference.json")["input_end_s"]
            run_dir = root / "attempt"
            dry(task, run_dir)
            with Mcp(sandbox_command(run_dir), run_dir) as client:
                setup(client, task)
                patch = read_json(run_dir / "final.json")["patches"]["in_1"]
                client.call("patch.set", {"instance": "in_1", "patch": patch})
                # A release the recipe does not have: a non-zero loss both paths
                # must report identically.
                client.call("patch.merge", {"instance": "in_1",
                                            "patch": {"params": {"amp.release": "400 ms"}}})
                rendered = client.call("render", dict(task["render"], notation=task["phrase"]))
                test_end = rendered["seconds"] - rendered["tail_s"]
                compared = client.call("compare", {"reference": str(reference),
                                                   "test": rendered["render"],
                                                   "reference_note_off_s": reference_end})
            scored = subprocess.run([sandbox_command(run_dir)[0], "eval-reference", json.dumps(
                {"reference": str(reference), "test": rendered["path"],
                 "reference_note_off_s": reference_end, "test_note_off_s": test_end})],
                capture_output=True, text=True, timeout=90, check=True)
            self.assertGreater(compared["loss"]["total"], 0)
            self.assertAlmostEqual(compared["loss"]["total"], json.loads(scored.stdout)["loss"],
                                   places=9)
            self.assertNotIn("note_off_assumed", compared["loss"])
            self.assertAlmostEqual(compared["loss"]["note_off_s"]["test"], test_end, places=9)

    def test_limits_capture_batch_and_invalid_final(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as directory:
            root = Path(directory)
            task = load_tasks("synth-descriptive", ["sub-bass"])[0]
            with Mcp(sandbox_command(root), root) as client:
                setup(client, task)
                recorder = Recorder(client, root, 1)
                message = {"jsonrpc": "2.0", "id": 100, "method": "tools/call", "params": {
                    "name": "batch", "arguments": {"steps": [
                        {"command": "patch.merge", "args": {"instance": "in_1",
                                                            "patch": {"params": {"amp.level": -3}}}},
                        {"command": "param.set", "args": {"instance": "in_1", "values": {"output": -9}}}]}}}
                self.assertFalse(recorder.handle(message)["result"].get("isError", False))
                final = read_json(root / "final.json")
                self.assertEqual(final["patches"]["in_1"]["params"]["amp.level"], -3)
                message["id"] = 101
                self.assertTrue(recorder.handle(message)["result"]["isError"])
                recorder.close()
                client.call("plugin.remove", {"instance": "in_1"})
                capture(client, root / "final.json")
            with self.assertRaisesRegex(ValueError, "Final chain"):
                score_final(task, root)


if __name__ == "__main__":
    unittest.main()
