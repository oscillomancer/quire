#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Smoke the packaged installation in an isolated user environment."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from common import ROOT


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("installed", type=Path)
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()
    product = json.loads((args.build_dir / "package/product.json").read_text())
    root = args.installed.resolve()
    slug = product["slug"]
    if sys.platform == "darwin":
        binary_dir = root / "Contents/MacOS"
        data = root / "Contents/Resources"
        plugins = root / "Contents/PlugIns/CLAP"
    elif os.name == "nt":
        binary_dir, data, plugins = root, root, root / "plugins/clap"
    else:
        binary_dir = root / "bin"
        data = root / "share" / slug
        plugins = root / "lib" / slug / "plugins"
    ext = ".exe" if os.name == "nt" else ""
    with tempfile.TemporaryDirectory(prefix="smoke-") as temp:
        user = Path(temp)
        env = dict(os.environ, HOME=temp, XDG_CONFIG_HOME=str(user / "config"),
                   XDG_CACHE_HOME=str(user / "cache"), XDG_DATA_HOME=str(user / "data"),
                   CLAP_PATH="", VST3_PATH="", SUPPLE_SCRIPT_LIBRARY="", ASAN_OPTIONS="detect_leaks=0")
        if os.name == "nt":
            env.update(USERPROFILE=temp, LOCALAPPDATA=str(user / "local"), APPDATA=str(user / "roaming"))
        def execute(suffix, *command, expected=0):
            result = subprocess.run([str(binary_dir / f"{slug}{suffix}{ext}"), *command],
                                    env=env, cwd=temp, capture_output=True, text=True, timeout=120)
            if result.returncode != expected:
                raise RuntimeError(f"{suffix}: exit {result.returncode}\n{result.stdout}\n{result.stderr}")
            return result
        help_result = execute("", "--help", expected=2)
        assert "usage:" in help_result.stderr.lower(), "GUI CLI output was lost"
        snapshot = user / "snapshot.png"
        execute("", "--offscreen", "--no-socket", "--audio", "dummy", "--frames", "5",
                "--snapshot", str(snapshot))
        assert snapshot.stat().st_size > 100, "GUI snapshot missing"
        # No --plugin-path: this verifies installed stockPluginDir lookup.
        result = execute("-engine", "exec", '{"command":"plugin.list","args":{}}',
                         "--workdir", temp)
        listing = json.loads(result.stdout)
        discovered = {Path(plugin["path"]).resolve() for plugin in listing["plugins"]}
        for plugin in plugins.glob("*.clap"):
            assert plugin.resolve() in discovered, f"{plugin.name} not discovered from the packaged location"
        assert len(list(plugins.glob("*.clap"))) == 14
        # A unique absent socket/instance must produce an actionable failure.
        result = execute("-mcp", "--instance", "package-smoke-absent", expected=1)
        assert "running" in result.stderr
        for suffix in ("-bridge", "-scanner"):
            assert (binary_dir / f"{slug}{suffix}{ext}").is_file()
        assert (data / "skill/SKILL.md").is_file()
        assert (data / "fonts/Inter-Regular.ttf").is_file()
    print("Packaged smoke: GUI help/snapshot, 14 plugins, MCP failure, helpers, skill, fonts passed")


if __name__ == "__main__":
    main()
