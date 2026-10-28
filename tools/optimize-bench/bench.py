#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference-search benchmark for `optimize.start` (see README.md).

For every synth-reference task: load the synth, set the task's starting patch,
render the hidden patch as the reference, then ask the optimizer to find it —
in two modes (`auto`: every tunable parameter of the patch's present modules;
`module`: only the tunable parameters of the modules the task changed) with a
fixed evaluation budget and several seeds. Records the initial and best loss,
evaluations, wall time, failed candidates and whether the best reached the
task's `max_loss`, and writes a JSON report plus a Markdown table.

Run:  python3 tools/optimize-bench/bench.py --label baseline
"""

import argparse
import concurrent.futures
import json
import statistics
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT / "tools/eval-harness"))

from common import SYNTH_NAME, Mcp, load_tasks, sandbox_command, setup, slug, write_json  # noqa: E402

DEFAULT_MODES = ("auto", "module")


def module_prefixes(changed_modules):
    """The parameter key prefix of each changed module: `amp` for the
    `amp.*` parameters (`envelopes.amp`, an older task's patch path, gives
    `amp` too).
    """
    return {module.split(".")[-1] for module in changed_modules}


def module_keys(auto_keys, changed_modules):
    """The "module" mode's keys: the changed modules' share of the auto list.

    Taking the subset of the optimizer's own "auto" result keeps the two modes
    comparable — same tunable and exclusion rules — and sidesteps
    `plugin.describe`, whose `active` flags are the schema captured at load
    (they do not follow a patch that adds a module, while the optimizer's live
    schema does).
    """
    prefixes = module_prefixes(changed_modules)
    return [key for key in auto_keys if key.split(".")[0] in prefixes]


def wait_job(client, job, poll_seconds=30.0):
    """Polls job.wait until the job leaves "running"; returns the last status."""
    while True:
        status = client.call("job.wait", {"job": job, "timeout_s": poll_seconds})
        if status.get("status") != "running":
            return status


def run_one(client, task, starting_patch, params, seed, options, mode):
    """One optimize.start, from the task's starting patch, until it stops."""
    client.call("patch.set", {"instance": "in_1", "patch": starting_patch})
    args = {"instance": "in_1", "params": params, "target": {"reference": "rd_1"},
            "phrase": task["phrase"], "evaluations": options.evaluations,
            "max_seconds": options.max_seconds, "seed": seed}
    if task["render"].get("tail_s") is not None:
        args["tail_s"] = task["render"]["tail_s"]
    record = {"task": task["id"], "mode": mode, "seed": seed,
              "dimension": len(params) if isinstance(params, list) else None,
              "params": params, "budget": options.evaluations}
    started = time.monotonic()
    try:
        started_job = client.call("optimize.start", args)
    except ValueError as error:
        record.update({"error": str(error), "seconds": time.monotonic() - started})
        return record
    status = wait_job(client, started_job["job"])
    record["seconds"] = round(time.monotonic() - started, 2)
    if status.get("status") != "done":
        record.update({"status": status.get("status"), "error": status.get("error")})
        return record
    result = status["result"]
    record.update({"initial": result["loss"]["initial"], "best": result["loss"]["best"],
                   "terms": result["loss"]["terms"], "evaluations": result["evaluations"],
                   "search_seconds": result["seconds"], "stopped": result["stopped"],
                   "failed": result.get("failed"), "restarts": result.get("restarts"),
                   "max_loss": task["score"]["max_loss"],
                   "pass": result["loss"]["best"] <= task["score"]["max_loss"]})
    return record


def run_task(task, options):
    """Every run of one task: the reference render once, then one session."""
    record = {"task": task["id"], "tier": task["tier"],
              "changed_modules": task.get("changed_modules") or [], "runs": []}
    starting_patch = task.get("starting_patch") or {}
    if not starting_patch:
        record["not_applicable"] = ("tier 3 starts from an empty patch (a structure task): the "
                                    "optimizer has no parameters to tune")
        return record
    workdir = ROOT / "build/optimize-bench" / f"{options.label}-{task['id']}"
    with Mcp(sandbox_command(workdir, options.sandbox), workdir) as client:
        setup(client, task)
        # The starting patch first: whether a parameter is tunable depends on the
        # modules the patch has (a filter-only task has no filter1 before this).
        client.call("patch.set", {"instance": "in_1", "patch": starting_patch})
        client.call("patch.set", {"instance": "in_1", "patch": task["hidden_patch"]})
        render = client.call("render", dict(task["render"], notation=task["phrase"]))
        record["reference"] = {"render": render["render"], "seconds": render["seconds"],
                               "tail_s": render.get("tail_s")}
        client.call("patch.set", {"instance": "in_1", "patch": starting_patch})
        # The optimizer's own list of tunable parameters (see module_keys).
        probe = {"instance": "in_1", "params": "auto", "target": {"reference": render["render"]},
                 "phrase": task["phrase"], "evaluations": 10, "max_seconds": 30, "seed": 0}
        if task["render"].get("tail_s") is not None:
            probe["tail_s"] = task["render"]["tail_s"]
        started = client.call("optimize.start", probe)
        client.call("job.cancel", {"job": started["job"]})
        record["auto_keys"] = started["params"]
        for mode in options.modes:
            params = ("auto" if mode == "auto"
                      else module_keys(record["auto_keys"], record["changed_modules"]))
            record.setdefault("param_keys", {})[mode] = params
            if isinstance(params, list) and not params:
                record["runs"].append({"task": task["id"], "mode": mode,
                                       "error": "no tunable parameters for the changed modules"})
                continue
            for seed in options.seeds:
                record["runs"].append(
                    run_one(client, task, starting_patch, params, seed, options, mode))
    return record


def summarize(records, options):
    """Per-mode aggregate: median/best loss, passes, evaluations, failures."""
    summary = {"label": options.label, "budget": options.evaluations, "seeds": options.seeds,
               "modes": {}, "not_applicable": [r["task"] for r in records
                                               if r.get("not_applicable")]}
    for mode in options.modes:
        runs = [run for r in records for run in r["runs"]
                if run.get("mode") == mode and "best" in run]
        # A mode with nothing to tune for a task is a skip, not a failed run.
        skips = [run for r in records for run in r["runs"]
                 if run.get("mode") == mode and "best" not in run
                 and str(run.get("error", "")).startswith("no tunable parameters")]
        errors = [run for r in records for run in r["runs"]
                  if run.get("mode") == mode and "best" not in run and run not in skips]
        losses = [run["best"] for run in runs]
        summary["modes"][mode] = {
            "runs": len(runs), "errors": len(errors), "skipped": len(skips),
            "median_best": round(statistics.median(losses), 4) if losses else None,
            "min_best": round(min(losses), 4) if losses else None,
            "max_best": round(max(losses), 4) if losses else None,
            "passes": sum(1 for run in runs if run.get("pass")),
            "median_evaluations": (statistics.median([run["evaluations"] for run in runs])
                                   if runs else None),
            "median_seconds": (round(statistics.median([run["seconds"] for run in runs]), 1)
                               if runs else None),
            "failed_candidates": sum(run.get("failed") or 0 for run in runs),
            "dimensions": sorted({run.get("dimension") for run in runs if run.get("dimension")}),
        }
    return summary


def markdown(records, summary, options):
    """One table per mode, then the per-task detail."""
    lines = [f"# Optimizer reference-search benchmark — {options.label}", "",
             f"{SYNTH_NAME}, {options.evaluations} evaluations per run, seeds "
             f"{', '.join(str(s) for s in options.seeds)}, {len(summary['modes'])} modes.", ""]
    if summary["not_applicable"]:
        lines += [f"Not applicable: {', '.join(summary['not_applicable'])} "
                  f"(tier 3 starts from an empty patch — a structure task).", ""]
    for record in records:
        if record.get("error"):
            lines += [f"Task `{record['task']}` failed: {record['error']}", ""]
    lines += ["| Mode | Runs | Median best | Min | Max | Pass (≤ max_loss) | Median evals | "
              "Median s | Skipped | Failed candidates | Dim |",
              "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|"]
    for mode, stats in summary["modes"].items():
        dims = ", ".join(str(d) for d in stats["dimensions"]) or "—"
        lines.append(f"| `{mode}` | {stats['runs']} | {stats['median_best']} | "
                     f"{stats['min_best']} | {stats['max_best']} | "
                     f"{stats['passes']}/{stats['runs']} | {stats['median_evaluations']} | "
                     f"{stats['median_seconds']} | {stats.get('skipped', 0)} | "
                     f"{stats['failed_candidates']} | {dims} |")
    lines += ["", "Per task and seed (`best` is the loss the optimizer stopped at; the "
              "reference is the task's own hidden patch):", "",
              "| Task | Tier | Mode | Dim | Seed | Initial | Best | Pass | Evals | Seconds | "
              "Stopped | Failed |", "|---|---:|---|---:|---:|---:|---:|:--:|---:|---:|---|---:|"]
    for record in records:
        task_rows = [run for run in record["runs"]]
        if not task_rows:
            continue
        for run in task_rows:
            if "best" not in run:
                lines.append(f"| {record['task']} | {record['tier']} | `{run.get('mode', '—')}` | "
                             f"— | {run.get('seed', '—')} | — | — | — | — | "
                             f"{run.get('seconds', '—')} | {run.get('error', run.get('status'))} | — |")
                continue
            lines.append(f"| {record['task']} | {record['tier']} | `{run['mode']}` | "
                         f"{run.get('dimension', '—')} | {run['seed']} | {run['initial']} | "
                         f"{run['best']} | {'yes' if run['pass'] else 'no'} | "
                         f"{run['evaluations']} | {run['seconds']} | {run['stopped']} | "
                         f"{run.get('failed', '—')} |")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", default="baseline", help="Name of this variant (file name).")
    parser.add_argument("--tasks", nargs="+", help="Task ids or suffixes; default: all.")
    parser.add_argument("--modes", nargs="+", default=list(DEFAULT_MODES),
                        choices=list(DEFAULT_MODES))
    parser.add_argument("--seeds", nargs="+", type=int, default=[1, 2, 3])
    parser.add_argument("--evaluations", type=int, default=300)
    parser.add_argument("--max-seconds", type=float, default=600.0)
    parser.add_argument("--jobs", type=int, default=4, help="Tasks in parallel; keep total CPU ≤ 6.")
    parser.add_argument("--sandbox", type=Path, help="Sandbox binary; default build/dev.")
    parser.add_argument("--out-dir", type=Path, default=ROOT / "docs/benchmarks/optimizer")
    options = parser.parse_args(argv)
    if not 1 <= options.jobs <= 6:
        parser.error("jobs must be 1..6 (the benchmark shares the machine)")
    options.out_dir = options.out_dir.resolve()
    if options.sandbox:
        options.sandbox = options.sandbox.resolve()

    tasks = load_tasks("synth-reference", options.tasks or None)
    started = time.monotonic()
    records = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=options.jobs) as pool:
        futures = {pool.submit(run_task, task, options): task for task in tasks}
        for future in concurrent.futures.as_completed(futures):
            task = futures[future]
            try:
                records.append(future.result())
            except Exception as error:  # a failed task must not lose the others
                records.append({"task": task["id"], "tier": task["tier"], "runs": [],
                                "error": f"{type(error).__name__}: {error}"})
            print(f"  {task['id']}: done ({time.monotonic() - started:.0f}s)", flush=True)
    records.sort(key=lambda r: r["task"])
    summary = summarize(records, options)
    summary["wall_seconds"] = round(time.monotonic() - started, 1)
    summary["sandbox"] = str(options.sandbox or (ROOT / "build/dev/host/sandbox" / (slug() + "-sandbox")))

    options.out_dir.mkdir(parents=True, exist_ok=True)
    json_path = options.out_dir / f"{options.label}.json"
    md_path = options.out_dir / f"{options.label}.md"
    write_json(json_path, {"summary": summary, "tasks": records})
    md_path.write_text(markdown(records, summary, options), encoding="utf-8")
    raw = ROOT / "build/optimize-bench" / f"{options.label}.jsonl"
    raw.parent.mkdir(parents=True, exist_ok=True)
    with raw.open("w") as out:
        for record in records:
            for run in record["runs"]:
                out.write(json.dumps(run) + "\n")
    print(json.dumps(summary, indent=2))
    print("\nwrote:")
    for path in (json_path, md_path, raw):
        print(f"  {path}")
    return int(any(record.get("error") or any(
        "best" not in run and not str(run.get("error", "")).startswith("no tunable parameters")
        for run in record["runs"]) for record in records))


if __name__ == "__main__":
    raise SystemExit(main())
