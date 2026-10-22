# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure helper and process-isolation tests; no REAPER or audio device required."""
from array import array
from contextlib import redirect_stdout
from io import StringIO
import importlib.util
import json
import math
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import wave

spec = importlib.util.spec_from_file_location("reaper_check", Path(__file__).with_name("check.py"))
check = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = check
spec.loader.exec_module(check)


class Helpers(unittest.TestCase):
    def test_config_guard_detects_content_metadata_and_tree_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            file = root / "reaper.ini"
            file.write_text("aaaa")
            before = check.snapshot(root)
            self.assertEqual(check.changes(before, check.snapshot(root)), [])
            stamp = file.stat().st_mtime_ns
            file.write_text("bbbb")
            os.utime(file, ns=(stamp, stamp))
            self.assertIn("reaper.ini", check.changes(before, check.snapshot(root)))
            before = check.snapshot(root)
            os.utime(file, ns=(stamp + 1, stamp + 1))
            self.assertIn("reaper.ini", check.changes(before, check.snapshot(root)))
            before = check.snapshot(root)
            file.unlink()
            self.assertIn("reaper.ini", check.changes(before, check.snapshot(root)))
            (root / "new").write_text("test")
            self.assertIn("new", check.changes(before, check.snapshot(root)))

    def test_private_config_copies_only_license(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            user = root / "user"
            user.mkdir()
            (user / "reaper-license.rk").write_text("test-only license")
            (user / "reaper.ini").write_text("clap_path_linux-x86_64=wrong\nsecret=do-not-copy")
            (user / "session.rpp").write_text("do-not-copy")
            before = check.snapshot(user)
            ini = check.private_config(root, root / "plugins", user)
            self.assertEqual(check.snapshot(user), before)
            text = ini.read_text()
            self.assertIn("linux_audio_mode=2", text)
            self.assertIn(f"clap_path_linux-x86_64={root / 'plugins'}", text)
            self.assertNotIn("secret", text)
            self.assertFalse((ini.parent / "session.rpp").exists())
            self.assertEqual((ini.parent / "reaper-license.rk").stat().st_mode & 0o777, 0o600)

    def test_wav_pcm_and_float_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "test.wav"
            with wave.open(str(path), "wb") as wav:
                wav.setparams((2, 2, check.RATE, 0, "NONE", ""))
                wav.writeframes(struct.pack("<h", 1024) * (check.RATE * check.SECONDS * 2))
            samples = check.read_wav(path)
            self.assertEqual(samples[0], 1024 / 32768)
            self.assertEqual(len(samples), check.RATE * check.SECONDS * 2)
            fmt = struct.pack("<HHIIHH", 3, 2, check.RATE, check.RATE * 8, 8, 32)
            audio = struct.pack("<f", .125) * (check.RATE * check.SECONDS * 2)
            def save(data):
                body = b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt
                body += b"JUNK\x01\x00\x00\x00x\x00"  # odd chunk padding
                body += b"data" + struct.pack("<I", len(data)) + data
                path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
            save(audio)
            self.assertEqual(check.read_wav(path)[-1], .125)
            save(struct.pack("<f", math.nan) + audio[4:])
            with self.assertRaisesRegex(check.CheckError, "non-finite"):
                check.read_wav(path)
            save(audio[:-8])
            with self.assertRaisesRegex(check.CheckError, "length"):
                check.read_wav(path)
            save(audio)
            path.write_bytes(path.read_bytes()[:-1])
            with self.assertRaisesRegex(check.CheckError, "truncated"):
                check.read_wav(path)
            fmt = struct.pack("<HHIIHH", 1, 2, check.RATE, 0, 0, 0)
            save(audio)
            with self.assertRaisesRegex(check.CheckError, "unexpected WAV format"):
                check.read_wav(path)

    def test_automation_measurement_cancels_input_level_changes(self):
        reference = array("d", [.1]) * (check.RATE * 4) + array("d", [.2]) * (check.RATE * 4)
        automated = array("d", [.1]) * (check.RATE * 4)
        automated.extend(array("d", [.2 * 10**(-12/20)]) * (check.RATE * 4))
        self.assertAlmostEqual(check.automation_delta(automated, reference), -12, places=8)

    def test_generated_scope_preserves_statements_and_bounds_locals(self):
        self.assertEqual(check.lua_scope(["local a = 1", "use(a)"]),
                         ["do", "local a = 1", "use(a)", "end"])

    def test_lua_paths_are_literals_not_code(self):
        value = '/a/"; error("injection") -- ]] [=[unicode-音]=]'
        quoted = check.lua_string(value)
        self.assertTrue(quoted.startswith("[==["))
        self.assertEqual(quoted[4:-4], value)


class EditorChecks(unittest.TestCase):
    def test_host_confirmation_requires_both_shown_and_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp) / "Hickory"
            folder.mkdir()
            class Runner:
                def run(self, args, label):
                    self.invocation = (args, label)
            runner = Runner()
            metadata = folder / "editor.tsv"
            metadata.write_text("shown\t1\nclosed\t1\ncomplete\t1\n")
            result = check.check_editor("com.suppleaudio.hickory", runner, folder, Path(tmp) / "config")
            self.assertEqual(result["closed"], "1")
            self.assertEqual(runner.invocation, ([str(folder / "editor.lua")], "Hickory-editor"))
            # A successful exit or complete marker alone cannot pass the check.
            for content in ("complete\t1\n", "shown\t1\ncomplete\t1\n",
                            "shown\t1\nclosed\t1\n", "error\tUI failed\n"):
                metadata.write_text(content)
                with self.assertRaises(check.CheckError):
                    check.check_editor("com.suppleaudio.hickory", runner, folder, Path(tmp) / "config")
            metadata.unlink()
            with self.assertRaisesRegex(check.CheckError, "editor did not produce metadata"):
                check.check_editor("com.suppleaudio.hickory", runner, folder, Path(tmp) / "config")

    def test_editor_is_checked_after_signal_failure_but_not_when_disabled(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            class Runner:
                def run(self, args, label):
                    if args[0] == "-renderproject":
                        Path(args[1]).with_suffix(".wav").touch()
            with patch.object(check, "read_metadata", return_value={"complete": "1"}), \
                    patch.object(check, "read_wav", return_value=[]), \
                    patch.object(check, "rms", return_value=0), \
                    patch.object(check, "difference", return_value=0), \
                    patch.object(check, "check_editor", return_value={"shown": "1", "closed": "1"}) as editor:
                row = check.check_plugin("com.suppleaudio.hickory", check.CASES["com.suppleaudio.hickory"],
                                         Runner(), root / "enabled", root / "config", root / "source", True)
                self.assertEqual((row["load"], row["signal"], row["editor"]), ("pass", "fail", "pass"))
                editor.assert_called_once()
                editor.reset_mock()
                editor.side_effect = check.CheckError("editor failed")
                row = check.check_plugin("com.suppleaudio.hickory", check.CASES["com.suppleaudio.hickory"],
                                         Runner(), root / "failed", root / "config", root / "source", True)
                self.assertEqual((row["signal"], row["editor"]), ("fail", "fail"))
                self.assertEqual(row["editor_reason"], "editor failed")
                self.assertEqual(row["editor_error"]["code"], "check.failed")
                editor.reset_mock()
                row = check.check_plugin("com.suppleaudio.hickory", check.CASES["com.suppleaudio.hickory"],
                                         Runner(), root / "disabled", root / "config", root / "source")
                self.assertEqual(row["editor"], "skipped")
                self.assertEqual(row["editor_reason"], "--editor not requested")
                editor.assert_not_called()

    def test_editor_failure_sets_nonzero_exit_and_preserves_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            plugins = root / "build/dev/plugins/clap"
            plugins.mkdir(parents=True)
            (plugins / "Hickory.clap").write_bytes(b"test artifact")
            row = {"plugin": "com.suppleaudio.hickory", "load": "pass", "signal": "pass",
                   "automation": "pass", "state": "pass", "editor": "fail", "reason": "UI failed"}
            with patch.object(check, "ROOT", root), patch.object(check.Path, "home", return_value=root), \
                    patch.object(check, "test_signal"), patch.object(check, "check_plugin", return_value=row) as plugin, \
                    redirect_stdout(StringIO()):
                self.assertEqual(check.main(["--build-dir", str(root / "build/dev"),
                                             "--reaper", "fake-reaper", "--editor"]), 1)
            self.assertTrue(plugin.call_args.args[-1])
            report = json.loads(next((root / "build/reaper-check").glob("*/report.json")).read_text())
            self.assertTrue(report["editor_requested"])
            self.assertEqual(report["results"][0]["editor"], "fail")
            self.assertTrue(report["user_config_unchanged"])


class ProcessIsolation(unittest.TestCase):
    def fixture(self, root, body):
        user, cfg, logs = root / "user", root / "cfg", root / "logs"
        for directory in (user, cfg, logs):
            directory.mkdir()
        ini = cfg / "reaper.ini"
        ini.write_text("[reaper]\n")
        executable = root / "fake-reaper"
        executable.write_text("#!" + sys.executable + "\n" + body)
        executable.chmod(0o700)
        return check.Runner(str(executable), ini, user, logs, .3), user, logs

    def test_audio_child_uses_only_requested_client_latency(self):
        with tempfile.TemporaryDirectory() as tmp:
            body = "import os,json\nprint(json.dumps(dict(os.environ)))\n"
            runner, _, logs = self.fixture(Path(tmp), body)
            runner.audio, runner.block = True, 128
            with patch.dict(os.environ, {"SUPPLE_AUDIO_DEVICE_TEST": "1"}, clear=True):
                runner.run([], "latency")
                child = json.loads((logs / "latency.log").read_text())
                self.assertEqual(child["PIPEWIRE_LATENCY"], "128/48000")
                self.assertNotIn("PIPEWIRE_QUANTUM", child)
                self.assertNotIn("PIPEWIRE_RATE", child)
                for name in ("PIPEWIRE_QUANTUM", "PIPEWIRE_RATE"):
                    with patch.dict(os.environ, {name: "64/48000"}), patch.object(check.subprocess, "Popen") as launch:
                        with self.assertRaisesRegex(check.CheckError, "unset PIPEWIRE"):
                            runner.run([], "forced")
                        launch.assert_not_called()

    def test_timeout_kills_parent_and_child(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            marker = root / "child.pid"
            body = ("import subprocess,sys,time\n"
                    "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)'])\n"
                    f"open({str(marker)!r},'w').write(str(child.pid))\n"
                    "time.sleep(30)\n")
            runner, _, logs = self.fixture(root, body)
            with self.assertRaisesRegex(check.CheckError, "timed out"):
                runner.run([], "timeout")
            pid = int(marker.read_text())
            proc = Path(f"/proc/{pid}/stat")
            # An orphan killed child may await init's reaping; it is not running.
            if proc.exists():
                self.assertEqual(proc.read_text().split(")", 1)[1].split()[0], "Z")
            self.assertEqual(json.loads((logs / "timeout.config-before.json").read_text()),
                             json.loads((logs / "timeout.config-after.json").read_text()))

    def test_config_mutation_fails_loudly(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            body = f"open({str(root / 'user/modified')!r},'w').write('changed')\n"
            runner, _, logs = self.fixture(root, body)
            with self.assertRaisesRegex(check.ConfigChanged, "USER CONFIG CHANGED"):
                runner.run([], "mutation")
            self.assertTrue((logs / "mutation.config-after.json").exists())

    def test_nonzero_exit_is_a_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            runner, _, _ = self.fixture(Path(tmp), "raise SystemExit(7)\n")
            with self.assertRaisesRegex(check.CheckError, "exit 7"):
                runner.run([], "nonzero")


if __name__ == "__main__":
    unittest.main()
