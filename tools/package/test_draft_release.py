# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the draft uploader with a fake gh; never contact GitHub."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / ".github/scripts/draft-release.sh"


@unittest.skipUnless(os.name == "posix" and shutil.which("bash") and shutil.which("jq"),
                     "Draft job runs on Linux with bash and jq")
class DraftRelease(unittest.TestCase):
    def run_upload(self, release, platforms="all"):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "dist").mkdir()
            (root / "dist/package.zip").write_text("fixture")
            (root / "release.json").write_text(json.dumps(release))
            gh = root / "gh"
            gh.write_text(f"#!{sys.executable}\n" + '''import json, sys
from pathlib import Path
with Path("calls.jsonl").open("a") as stream:
    stream.write(json.dumps(sys.argv[1:]) + "\\n")
if sys.argv[1:3] == ["release", "view"]:
    release = json.loads(Path("release.json").read_text())
    if release is None:
        sys.exit(1)
    print(json.dumps(release))
''')
            gh.chmod(0o755)
            result = subprocess.run(["bash", str(SCRIPT)], cwd=root, text=True,
                                    capture_output=True, env=dict(os.environ,
                                    PATH=str(root) + os.pathsep + os.environ["PATH"],
                                    VERSION="1.2.3", GITHUB_SHA="commit-a", PLATFORMS=platforms))
            calls = root / "calls.jsonl"
            return result, [json.loads(line) for line in calls.read_text().splitlines()] if calls.exists() else []

    def test_partial_platforms_never_mutate_a_release(self):
        for release in (None, {"isDraft": True, "targetCommitish": "commit-a"}):
            with self.subTest(release=release):
                result, calls = self.run_upload(release, "windows")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("all platforms", result.stderr)
                self.assertEqual(calls, [])

    def test_wrong_revision_and_published_releases_are_untouched(self):
        for release in ({"isDraft": True, "targetCommitish": "commit-b"},
                        {"isDraft": False, "targetCommitish": "commit-a"}):
            with self.subTest(release=release):
                result, calls = self.run_upload(release)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual([c[1] for c in calls], ["view"])
                if release["isDraft"]:
                    self.assertIn("Delete the draft (or bump the version)", result.stderr)

    def test_all_platforms_create_or_replace_only_at_the_same_revision(self):
        for release in (None, {"isDraft": True, "targetCommitish": "commit-a"}):
            with self.subTest(release=release):
                result, calls = self.run_upload(release)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([c[1] for c in calls],
                                 ["view", "create", "upload"] if release is None else ["view", "upload"])
                self.assertIn("dist/package.zip", calls[-1])
                if release is None:
                    self.assertEqual(calls[1][calls[1].index("--target") + 1], "commit-a")
