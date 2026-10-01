#!/usr/bin/env python3
"""P3.4 evaluation: defaults to a free scripted validation run."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time

from common import HERE, ROOT, Mcp, evaluation_profile, load_tasks, read_json, recipe_patch, sandbox_command, setup, skill_hash, write_json
import runners
from scoring import notation_score, transcript_metrics


def render_patch(task, patch, directory, seed, binary=None):
    with Mcp(sandbox_command(directory, binary), directory) as client:
        setup(client, task, seed)
        client.call("patch.set", {"instance": "in_1", "patch": dict(patch, seed=seed)})
        rendered = client.call("render", dict(task["render"], notation=task["phrase"]))
        report = client.call("analyze", {"source": rendered["render"], "detail": "full"})["report"]
        write_json(Path(directory) / "reference.json", {"input_end_s": rendered["seconds"] - rendered["tail_s"]})
        return Path(rendered["path"]), report


def score_final(task, directory, seed, binary=None, reference=None):
    directory = Path(directory)
    entries = [json.loads(line) for line in (directory / "tools.jsonl").read_text().splitlines()]
    metrics = transcript_metrics(entries)
    final = read_json(directory / "final.json")
    session = final["session"]
    # Added effects, missing instances and unsupported state are invalid, not
    # silently discarded: the task evaluates the one fixed instrument.
    if len(session["chain"]) != 1 or session["instances"][session["chain"][0]]["plugin"] != "com.suppleaudio.orrery":
        raise ValueError("Final chain must contain only the task's Orrery instance")
    patch = final["patches"][session["chain"][0]]
    if task["suite"] == "notation":
        attempts = metrics["notation_attempts"]
        if not attempts:
            raise ValueError("No notation was submitted through render")
        phrase = attempts[-1]["notation"]
    else:
        phrase = task["phrase"]
    scorer_dir = directory.parent / (directory.name + "-score")
    with Mcp(sandbox_command(scorer_dir, binary), scorer_dir) as client:
        setup(client, task, seed)
        client.call("patch.set", {"instance": "in_1", "patch": dict(patch, seed=seed)})
        rendered = client.call("render", dict(task["render"], notation=phrase))
        report = client.call("analyze", {"source": rendered["render"], "detail": "full"})["report"]
        path = Path(rendered["path"])
        if task["suite"] == "synth-descriptive":
            score = client.call("profile.evaluate", {"source": rendered["render"],
                                                     "profile": task["score"]["profile"]})["result"]
            score["pass_fraction"] = sum(t["pass"] for t in score["targets"]) / len(score["targets"])
            score["profile_loss"] = score["loss"]
            # Optimizer profile loss squares each distance; core-analysis also
            # reports its weighted mean normalized distance above.
            targets = task["score"]["profile"]["targets"]
            score["loss"] = sum(targets[t["key"]].get("weight", 1) * t["distance"] ** 2
                                for t in score["targets"])
        elif task["suite"] == "synth-reference":
            arguments = {"reference": str(reference), "test": str(path),
                         "reference_note_off_s": read_json(reference.parent.parent / "reference.json")["input_end_s"],
                         "test_note_off_s": rendered["seconds"] - rendered["tail_s"]}
            binary_path = sandbox_command(scorer_dir, binary)[0]
            result = subprocess.run([binary_path, "eval-reference", json.dumps(arguments)],
                                    capture_output=True, text=True, timeout=90)
            data = json.loads(result.stdout)
            if result.returncode:
                raise ValueError(data)
            score = dict(data, **{"pass": data["loss"] <= task["score"]["max_loss"]})
            score["pass_fraction"] = float(score["pass"])
        else:
            midi = read_json(path.with_suffix(".midi.json"))
            score = notation_score(midi, task["score"]["structure"], metrics["notation_attempts"])
    write_json(directory / "analysis.json", report)
    return {"valid_patch": True, "score": score, "efficiency": metrics, "render": str(path),
            "render_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "final_patch": patch, "final_notation": phrase}


def publish(report):
    model = re.sub(r"[^a-zA-Z0-9.-]+", "-", report["model"])
    date = report["started_utc"][:10]
    basename = f"{date}-{report['suite']}-{report['runner']}-{model}"
    destination = ROOT / "docs/benchmarks/eval" / (basename + ".json")
    if destination.exists():
        basename += "-" + report["run_id"].split("-")[0]
        destination = destination.with_name(basename + ".json")
    audio_dir = ROOT / "docs/benchmarks/eval/audio" / report["run_id"]
    rows = ["| Task | Repeat | Valid | Pass | Loss | Calls | Seconds |", "|---|---:|---|---|---:|---:|---:|"]
    for record in report["tasks"]:
        score = record.get("score", {})
        rows.append(f"| {record['task']} | {record['repeat']} | {record.get('valid_patch', False)} | "
                    f"{score.get('pass', False)} | {score.get('loss', '—')} | "
                    f"{record.get('efficiency', {}).get('tool_calls', 0)} | {record['wall_seconds']:.2f} |")
    listening = []
    for task_id in sorted({r["task"] for r in report["tasks"]}):
        records = [r for r in report["tasks"] if r["task"] == task_id and r.get("render")]
        if not records:
            continue
        ordered = sorted(records, key=lambda r: r["score"]["loss"])
        paths = []
        for label, record in (("best", ordered[0]), ("worst", ordered[-1])):
            output = audio_dir / f"{task_id}-{label}.wav"
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(record["render"], output)
            paths.append(str(output.relative_to(ROOT)))
        listening.append(f"- `{task_id}`: `{paths[0]}`, `{paths[1]}` — {records[0]['listening']}")
    report["listening_audio"] = str(audio_dir.relative_to(ROOT))
    write_json(destination, report)
    markdown = (f"# {report['suite']} — {report['runner']} / {report['model']}\n\n"
                f"Run `{report['run_id']}`; git `{report['git_hash']}`; skill `{report['skill_hash']}`.\n\n"
                f"Pass rate: {report['pass_rate']:.1%}. Median loss: {report['median_loss']}.\n\n"
                + "\n".join(rows) + "\n\nUsage: `" + json.dumps(report["usage_totals"]) + "`.\n\n"
                + "\n".join(listening) + "\n")
    destination.with_suffix(".md").write_text(markdown)
    history = ROOT / "docs/benchmarks/synth-eval.md"
    if not history.exists():
        history.write_text("# Synth and notation evaluation history\n\n"
                           "Dry validation is infrastructure coverage, not evidence of model quality.\n\n"
                           "| Date | Suite | Git | Skill SHA256 | Runner/model | Pass rate | Median loss | Report |\n"
                           "|---|---|---|---|---|---:|---:|---|\n")
    with history.open("a") as stream:
        stream.write(f"| {date} | {report['suite']} | `{report['git_hash'][:10]}` | `{report['skill_hash'][:12]}` | "
                     f"{report['runner']}/{report['model']} | {report['pass_rate']:.1%} | {report['median_loss']} | "
                     f"[{basename}](eval/{destination.name}) |\n")
    if report["runner"] == "dry":
        return destination  # Scripted answers are infrastructure coverage, not listening material.
    review = ROOT / "HUMAN_REVIEW.md"
    text = review.read_text()
    ids = [int(n) for n in re.findall(r"(?m)^### (\d+)\.", text)]
    item = (f"### {max(ids, default=0) + 1}. Evaluation listening — {report['run_id']}\n"
            f"Suite: {report['suite']}, runner/model: {report['runner']}/{report['model']}. "
            "Do the renders fit these descriptions, and do the numeric best/worst rankings match your ears?\n\n"
            + "\n".join(listening or ["- No valid renders; inspect the report's failures."]) + "\n\n")
    # Keep new items in the active queue, ahead of resolved decisions.
    review.write_text(text.replace("## Resolved", item + "## Resolved", 1))
    return destination


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["synth-descriptive", "synth-reference", "notation"], default="synth-descriptive")
    parser.add_argument("--runner", choices=["hermes", "dry"], default="dry")
    parser.add_argument("--dry-run", action="store_true", help="Validate with scripted answers, never call a model (default)")
    parser.add_argument("--tasks", nargs="+", help="Task ids or suffixes, space or comma separated")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--sandbox", type=Path)
    parser.add_argument("--hermes-bin", default="hermes")
    parser.add_argument("--hermes-python", help="Existing Hermes venv Python; invokes the same CLI without managing the user's install")
    parser.add_argument("--model-config", default=str(evaluation_profile() / "config.yaml"),
                        help="Dedicated evaluation profile model config; no default-profile fallback")
    parser.add_argument("--credentials", default=str(evaluation_profile() / ".env"),
                        help="Dedicated evaluation profile credential .env to reference; never copied")
    parser.add_argument("--publish", action="store_true", help="Write benchmarks and one listening queue item; default for real Hermes runs")
    options = parser.parse_args(argv)
    if not 1 <= options.repeat <= 100 or not 0 <= options.seed <= 1000000000:
        parser.error("repeat must be 1..100 and seed 0..1e9")
    runner = "dry" if options.dry_run else options.runner
    selected = [part for arg in (options.tasks or []) for part in arg.split(",")]
    tasks = load_tasks(options.suite, selected)
    start = datetime.now(timezone.utc)
    run_id = start.strftime("%Y%m%dT%H%M%S%fZ") + "-" + options.suite + "-" + runner
    run_dir = ROOT / "build/eval" / run_id
    run_dir.mkdir(parents=True)
    report = {"schema": "supple.eval-report/1.0", "run_id": run_id, "started_utc": start.isoformat(),
              "suite": options.suite, "runner": runner, "seed": options.seed,
              "git_hash": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
              "working_tree_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)),
              "skill_hash": skill_hash(), "task_hashes": {}, "tasks": [], "run_directory": str(run_dir.relative_to(ROOT))}
    if runner == "hermes":
        report["hermes_profile"] = Path(options.model_config).expanduser().parent.name
        report["model_config"] = str(Path(options.model_config).expanduser())
    usage_totals = {"total_tokens": 0, "estimated_cost_usd": 0.0, "cost_known": True}
    for task in tasks:
        report["task_hashes"][task["id"]] = hashlib.sha256(json.dumps(task, sort_keys=True).encode()).hexdigest()
        reference = None
        if task["suite"] == "synth-reference":
            reference, _ = render_patch(task, task["hidden_patch"], run_dir / (task["id"] + "-hidden"), options.seed, options.sandbox)
        for repeat in range(options.repeat):
            directory = run_dir / (task["id"] + f"-{repeat + 1}")
            directory.mkdir()
            begin = time.monotonic()
            record = {"task": task["id"], "repeat": repeat + 1, "seed": options.seed,
                      "listening": task["listening"], "transcript": str((directory / "transcript.jsonl").relative_to(ROOT))}
            try:
                record["runner"] = (runners.dry(task, directory, options.seed, options.sandbox)
                                    if runner == "dry" else runners.hermes(task, directory, options.seed, options, reference))
                record.update(score_final(task, directory, options.seed, options.sandbox, reference))
                record["limits_exceeded"] = record["efficiency"]["tool_calls"] > task["limits"]["tool_calls"]
                if record["runner"]["exit_code"] or record["runner"]["timed_out"] or record["limits_exceeded"]:
                    record["score"]["pass"] = False
                    record["incomplete"] = True
                if runner == "dry" and not record["score"]["pass"]:
                    record["error"] = "Scripted reference failed its own scorer/profile"
            except (ValueError, KeyError, OSError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
                record.update(valid_patch=False, error=str(error))
            usage = read_json(directory / "usage.json") if (directory / "usage.json").exists() else {}
            record["usage"] = usage
            total = usage.get("total_including_auxiliary", usage)
            usage_totals["total_tokens"] += total.get("total_tokens", 0) or 0
            cost = total.get("estimated_cost_usd")
            if cost is None:
                usage_totals["cost_known"] = False
            else:
                usage_totals["estimated_cost_usd"] += cost
            record["wall_seconds"] = round(time.monotonic() - begin, 3)
            report["tasks"].append(record)
            print(f"{task['id']} repeat {repeat + 1}: {'PASS' if record.get('score', {}).get('pass') else 'FAIL'}"
                  f" ({record['wall_seconds']:.2f}s)" + (f" — {record['error']}" if record.get("error") else ""), flush=True)
    report["model"] = next((r.get("usage", {}).get("model") or r.get("runner", {}).get("model")
                             for r in report["tasks"] if r.get("usage", {}).get("model") or r.get("runner", {}).get("model")), "unknown")
    report["provider"] = next((r.get("usage", {}).get("provider") for r in report["tasks"] if r.get("usage", {}).get("provider")), "unknown")
    report["usage_totals"] = usage_totals
    report["pass_rate"] = sum(r.get("score", {}).get("pass", False) for r in report["tasks"]) / len(report["tasks"])
    losses = [r["score"]["loss"] for r in report["tasks"] if r.get("score")]
    report["median_loss"] = statistics.median(losses) if losses else None
    write_json(run_dir / "report.json", report)
    if runner == "hermes" or options.publish:
        path = publish(report)
        print("Report:", path.relative_to(ROOT))
    else:
        print("Validation report:", run_dir / "report.json")
    if runner == "dry":
        return int(any(r.get("error") or not r.get("score", {}).get("pass") for r in report["tasks"]))
    return int(any(r.get("error") or r.get("incomplete") for r in report["tasks"]))


if __name__ == "__main__":
    raise SystemExit(main())
