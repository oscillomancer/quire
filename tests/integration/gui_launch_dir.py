#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The app starts from a launch directory it cannot write (Finder starts apps
in the read-only "/") and leaves nothing there. Offscreen, no audio device."""
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    app = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="supple-launch-") as temporary:
        user = Path(temporary)
        launch = user / "launch"
        launch.mkdir()
        # The user's folders live elsewhere, as for an app started by a launcher.
        env = dict(os.environ, HOME=temporary, XDG_CONFIG_HOME=str(user / "config"),
                   XDG_CACHE_HOME=str(user / "cache"), XDG_DATA_HOME=str(user / "data"),
                   CLAP_PATH="", VST3_PATH="")
        if os.name == "nt":
            env.update(USERPROFILE=temporary, LOCALAPPDATA=str(user / "local"),
                       APPDATA=str(user / "roaming"))
        launch.chmod(stat.S_IRUSR | stat.S_IXUSR)
        try:
            result = subprocess.run(
                [app, "--offscreen", "--no-socket", "--frames", "2", "--audio", "none"],
                cwd=launch, env=env, timeout=120, stderr=subprocess.PIPE, text=True)
        finally:
            launch.chmod(stat.S_IRWXU)
        assert result.returncode == 0, f"exit {result.returncode}: {result.stderr}"
        left = sorted(p.name for p in launch.iterdir())
        assert not left, f"the launch directory collected {left}"
    print("launch directory: ok")


if __name__ == "__main__":
    main()
