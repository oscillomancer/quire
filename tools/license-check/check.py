#!/usr/bin/env python3
"""License gate for the monorepo.

Fails (exit 1) when:
  * a directory under third_party/ has no manifest entry, or vice versa;
  * a manifest entry's license is not on the allow-list ("A OR B" passes
    when at least one alternative is allowed; "A AND B" requires all);
  * a declared license file is missing;
  * a dependency is not mentioned in THIRD_PARTY.md;
  * a CMake FetchContent_Declare / ExternalProject_Add names a dependency that
    is not in the manifest;
  * first-party source contains a GPL/AGPL/LGPL SPDX identifier.

Run: tools/license-check/check.py --root <repo>
"""
import argparse
import json
import pathlib
import re
import sys

FIRST_PARTY_DIRS = ["libs", "host", "plugins", "daw", "mcp", "tools", "tests"]
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh", ".m", ".mm", ".py", ".lua", ".cmake"}
# Built by concatenation so this file does not match its own pattern.
FORBIDDEN_SPDX = re.compile("SPDX-License-" + r"Identifier:[ \t]*[^\n]*\b(A?GPL|LGPL)", re.IGNORECASE)
FETCH_RE = re.compile(r"(?:FetchContent_Declare|ExternalProject_Add)\s*\(\s*([A-Za-z0-9_.+-]+)")
SKIP_PARTS = {"build", "_deps", ".git", "third_party", "node_modules"}


def license_ok(expr: str, allowed: set) -> bool:
    expr = expr.strip().strip("()")
    if " OR " in expr:
        return any(license_ok(p, allowed) for p in expr.split(" OR "))
    if " AND " in expr:
        return all(license_ok(p, allowed) for p in expr.split(" AND "))
    return expr in allowed


def first_party_files(root: pathlib.Path):
    for d in FIRST_PARTY_DIRS:
        base = root / d
        if not base.exists():
            continue
        for p in base.rglob("*"):
            if not p.is_file() or SKIP_PARTS.intersection(p.relative_to(root).parts):
                continue
            yield p


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parents[2]))
    args = ap.parse_args()
    root = pathlib.Path(args.root).resolve()
    errors = []

    manifest_path = root / "third_party" / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    allowed = set(manifest["allowed_licenses"])
    deps = manifest["dependencies"]
    by_dir = {d["dir"]: d for d in deps if d.get("dir")}
    names = {d["name"].lower() for d in deps}
    third_party_md = (root / "THIRD_PARTY.md").read_text() if (root / "THIRD_PARTY.md").exists() else ""
    if not third_party_md:
        errors.append("THIRD_PARTY.md is missing")

    tp = root / "third_party"
    for child in sorted(tp.iterdir()):
        if child.is_dir() and child.name not in by_dir:
            errors.append(f"third_party/{child.name}: no entry in third_party/manifest.json")

    for d in deps:
        tag = d["name"]
        if not license_ok(d["license"], allowed):
            errors.append(f"{tag}: license '{d['license']}' is not on the allow-list "
                          f"(needs the maintainer's approval and a manifest exception)")
        if d.get("dir") and not (tp / d["dir"]).is_dir():
            errors.append(f"{tag}: manifest dir third_party/{d['dir']} does not exist")
        lf = d.get("license_file")
        if lf and not (tp / lf).is_file():
            errors.append(f"{tag}: license file third_party/{lf} is missing")
        if d.get("fetched"):
            # Fetched at configure time on some platforms only (never in the
            # default offline build): must still declare license and pin.
            if not d.get("commit") or not d.get("source"):
                errors.append(f"{tag}: fetched dependency needs 'source' and a pinned 'commit'")
        elif not lf and d.get("linkage") != "system":
            errors.append(f"{tag}: no license_file declared")
        if tag.lower() not in third_party_md.lower():
            errors.append(f"{tag}: not described in THIRD_PARTY.md")

    for p in first_party_files(root):
        rel = p.relative_to(root)
        if p.suffix in SOURCE_SUFFIXES or p.name == "CMakeLists.txt":
            try:
                text = p.read_text(errors="replace")
            except OSError:
                continue
            if FORBIDDEN_SPDX.search(text):
                errors.append(f"{rel}: carries a (L/A)GPL SPDX identifier")
            if p.name == "CMakeLists.txt" or p.suffix == ".cmake":
                for m in FETCH_RE.finditer(text):
                    dep = m.group(1).lower()
                    # Template-derived plugins are local first-party sources,
                    # built in isolation to avoid CMake target collisions.
                    if (dep == "plugin-" and rel == pathlib.Path("plugins/CMakeLists.txt")
                            and "SOURCE_DIR \"${CMAKE_CURRENT_SOURCE_DIR}/${dir}\""
                            in text[m.end():m.end() + 160]):
                        continue
                    if dep not in names:
                        errors.append(f"{rel}: fetches '{m.group(1)}', which is not in third_party/manifest.json")

    if errors:
        print("license-check: FAILED", file=sys.stderr)
        for e in errors:
            print("  - " + e, file=sys.stderr)
        return 1
    print(f"license-check: OK ({len(deps)} dependencies)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
