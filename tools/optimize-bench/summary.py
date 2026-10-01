#!/usr/bin/env python3
"""Comparison table over optimize-bench reports (see README.md).

Reads one or more `<label>.json` reports written by bench.py and prints (or
writes with --out) the variants side by side, plus a per-task view of the best
loss for each variant and mode.

Run:  python3 tools/optimize-bench/summary.py docs/benchmarks/optimizer/2026-10-01-*.json
"""

import argparse
import json
import sys
from pathlib import Path


def load(path):
    data = json.loads(Path(path).read_text())
    return {"label": data["summary"]["label"], "summary": data["summary"],
            "tasks": {task["task"]: task for task in data["tasks"]}}


def variant_table(reports):
    lines = ["| Variant | Mode | Runs | Median best | Min | Max | Pass (≤ max_loss) | "
             "Median evals | Median s | Failed candidates |",
             "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for report in reports:
        for mode, stats in report["summary"]["modes"].items():
            lines.append(f"| `{report['label']}` | `{mode}` | {stats['runs']} | "
                         f"{stats['median_best']} | {stats['min_best']} | {stats['max_best']} | "
                         f"{stats['passes']}/{stats['runs']} | {stats['median_evaluations']} | "
                         f"{stats['median_seconds']} | {stats['failed_candidates']} |")
    return lines


def task_table(reports):
    """Median best loss of the three seeds per task and mode, one row per task."""
    lines = []
    for mode in sorted({mode for report in reports for mode in report["summary"]["modes"]}):
        lines += [f"| Task | Tier | {' | '.join('`' + r['label'] + '`' for r in reports)} |",
                  f"|---|---:|{'---:|' * len(reports)}"]
        tasks = sorted({task for report in reports for task in report["tasks"]})
        for task in tasks:
            row = []
            tier = "—"
            for report in reports:
                record = report["tasks"].get(task)
                if record is None:
                    row.append("—")
                    continue
                tier = record["tier"]
                runs = [run["best"] for run in record["runs"]
                        if run.get("mode") == mode and "best" in run]
                if not runs:
                    row.append(record.get("not_applicable") and "n/a" or "—")
                else:
                    row.append(f"{sorted(runs)[len(runs) // 2]:.3f}")
            lines.append(f"| {task} | {tier} | {' | '.join(row)} |")
        lines.append("")
    return lines


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", help="bench.py report JSON files")
    parser.add_argument("--out", type=Path, help="Write the table here instead of stdout.")
    options = parser.parse_args(argv)
    reports = [load(path) for path in sorted(options.reports)]
    lines = ["# Optimizer benchmark — variants", "",
             "Best loss per run of `optimize.start` against each task's hidden reference; "
             "`pass` is best ≤ the task's `max_loss`.", ""]
    lines += variant_table(reports)
    lines += ["", "Median best loss per task (over the run's seeds):", ""]
    lines += task_table(reports)
    text = "\n".join(lines).rstrip() + "\n"
    if options.out:
        options.out.write_text(text, encoding="utf-8")
        print(f"wrote {options.out}")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
