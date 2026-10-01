"""Capacity/report and REAPER generator regressions; no audio device required."""
import base64
import importlib.util
import json
from contextlib import redirect_stderr
from io import StringIO
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("capacity_bench", Path(__file__).with_name("run.py"))
bench = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = bench
spec.loader.exec_module(bench)


class Bench(unittest.TestCase):
    def test_search_bisects_and_independently_confirms(self):
        calls = []
        def measure(n, confirm):
            calls.append((n, confirm))
            return {"valid": True, "xruns": int(n > 19)}
        result = bench.capacity_search(measure, 100)
        self.assertEqual(result, {"tracks": 19, "confirmed": True, "capped": False, "first_failing_bound": 20})
        self.assertEqual(calls[-1], (19, True))
        self.assertLess(len(calls), 13)

    def test_capped_zero_capacity_and_failed_confirmation(self):
        self.assertEqual(bench.capacity_search(lambda n, c: {"valid": True, "xruns": 0}, 3),
                         {"tracks": 3, "confirmed": True, "capped": True, "first_failing_bound": None})
        self.assertEqual(bench.capacity_search(lambda n, c: {"valid": True, "xruns": 1}, 3)["tracks"], 0)
        result = bench.capacity_search(lambda n, c: {"valid": True, "xruns": int(c)}, 3)
        self.assertFalse(result["confirmed"])
        for key in ("xruns", "media_xruns", "anticipation_misses", "process_errors"):
            self.assertFalse(bench.zero_xruns({"valid": True, key: 1}))
        with self.assertRaises(bench.check.CheckError):
            bench.capacity_search(lambda n, c: {"valid": False}, 3)

    def test_report_deterministic_append_escaping_and_failed_run(self):
        report = {"schema": "supple.capacity-report/1.0", "timestamp": "time", "host": "b1", "device": "paced",
                  "anticipative": True, "directory": "a|b", "facts": {"kernel": "test", "pipewire": "line\nbreak",
                  "governors": {"cpu0": "powersave"}, "revision": "abcdef"},
                  "runs": [{"tracks": 3, "valid": True, "seconds": .1, "xruns": 0, "sample_rate": 48000, "quantum": 64}],
                  "search": {"tracks": 3, "confirmed": True, "capped": True}}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            markdown = path / "report.md"
            bench.save_report(report, path, markdown)
            encoded = (path / "report.json").read_bytes()
            bench.save_report(report, path, markdown)
            self.assertEqual(encoded, (path / "report.json").read_bytes())
            self.assertEqual(markdown.read_text().count("| UTC | host |"), 1)
            self.assertEqual(markdown.read_text().count("| >=3 |"), 2)
            self.assertIn("a\\|b", markdown.read_text())
            self.assertIn("line / break", markdown.read_text())
            del report["search"]
            self.assertIn("| 3 | not searched |", bench.markdown_row(report))
            report["runs"][0]["offline"] = True
            self.assertIn("| 3 | offline |", bench.markdown_row(report))
            report["runs"] = []
            report["error"] = {"message": "failed"}
            bench.save_report(report, path, markdown)
            self.assertEqual(json.loads((path / "report.json").read_text())["error"]["message"], "failed")

    def test_rpp_setup_uses_exported_midi_and_opaque_states_for_every_fx(self):
        entry = {"name": "Orrery", "plugin": "com.suppleaudio.orrery", "state": base64.b64encode(b"opaque\0state").decode()}
        session = {"pan_law": .70794578, "loop_seconds": 8, "tracks": [
            {"name": "playback", "volume": .1, "armed": False, "fx": [entry]*3,
             "notes": [{"start": 0, "end": 4, "key": 48, "velocity": 90}]},
            {"name": "live", "volume": .1, "armed": True, "fx": [entry]*3, "notes": []}], "master": [entry]}
        script = bench.setup_script(session, Path("/test/project"), Path("/test/config"))
        self.assertEqual(script.count("'clap_chunk'"), 14)
        self.assertEqual(script.count("unexpected CLAP identity"), 7)
        self.assertEqual(script.count("MIDI_InsertNote"), 1)
        self.assertIn("MIDI_GetPPQPosFromProjQN(take,4)", script)
        self.assertIn("'I_RECINPUT',1024", script)
        self.assertIn("'I_RECMON',1", script)
        self.assertIn("capacity.RPP", script)
        self.assertIn("'dummy'", script)
        self.assertIn("Audio_Quit", script)
        self.assertNotIn("Audio_Init", script)
        measurement = bench.measurement_script(Path("/project"), Path("/config"), 300)
        self.assertIn("GetUnderrunTime", measurement)
        self.assertIn("max_poll_gap_seconds", measurement)
        self.assertIn("expected JACK audio", measurement)
        self.assertIn("SUPPLE_AUDIO_DEVICE_TEST", measurement)

    def test_exact_opt_in_before_launch_or_private_real_config(self):
        for value in (None, "", "0", "true", "01", "1 "):
            env = {} if value is None else {"SUPPLE_AUDIO_DEVICE_TEST": value}
            with patch.dict(os.environ, env, clear=True), patch.object(bench.subprocess, "Popen") as launch:
                with self.assertRaises(SystemExit), redirect_stderr(StringIO()):
                    bench.main(["b1", "--device", "jack"])
                with tempfile.TemporaryDirectory() as tmp:
                    directory = Path(tmp)
                    with self.assertRaises(bench.check.CheckError):
                        bench.check.private_config(directory, directory, directory, device="jack")
                    runner = bench.check.Runner("reaper", directory / "reaper.ini", directory, directory, 1, audio=True)
                    with self.assertRaises(bench.check.CheckError):
                        runner.run([], "never")
                launch.assert_not_called()
        with patch.dict(os.environ, {"SUPPLE_AUDIO_DEVICE_TEST": "1"}):
            with tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                ini = bench.check.private_config(directory, directory, directory, block=64, device="jack")
                self.assertIn("linux_audio_mode=1", ini.read_text())
                self.assertIn("linux_audio_bsize=64", ini.read_text())
                self.assertIn("linux_audio_nch_in=2", ini.read_text())


if __name__ == "__main__":
    unittest.main()
