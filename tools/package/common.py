# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared staging and identity; requires an already built Release tree."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def options(platform):
    parser = argparse.ArgumentParser(description=f"Package the {platform} Release build")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build" / "release")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "dist")
    args = parser.parse_args()
    args.build_dir = args.build_dir.resolve()
    args.output_dir = args.output_dir.resolve()
    product = json.loads((args.build_dir / "package/product.json").read_text())
    args.output_dir.mkdir(parents=True, exist_ok=True)
    return args, product


def stage(build, dest):
    for component in ("Runtime", "Plugins"):
        run("cmake", "--install", build, "--prefix", dest, "--component", component)
    return dest


def archive_zip(tree, output):
    shutil.make_archive(str(output.with_suffix("")), "zip", tree)
