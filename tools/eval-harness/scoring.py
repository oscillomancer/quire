# SPDX-License-Identifier: GPL-3.0-or-later
"""Scoring over authoritative compiler/analysis output."""

from relay import invocations
from common import tool_data


def notation_score(midi, structure, attempts):
    numerator, denominator = map(int, midi["meter"].split("/"))
    ticks_per_bar = midi["ppq"] * numerator * 4 / denominator
    bars = midi["length_ticks"] / ticks_per_bar
    pitches = [n["pitch"] for n in midi["notes"]]
    density = len(pitches) / bars if bars else 0
    checks = {
        "first_attempt": bool(attempts) and attempts[0]["compiled"],
        "bars": bars == structure["bars"],
        "meter": midi["meter"] == structure["meter"] and all(
            m == structure["meter"] for m in midi.get("bar_meters", [])),
        "pitch_range": bool(pitches) and min(pitches) >= structure["pitch_range"][0]
                       and max(pitches) <= structure["pitch_range"][1],
        "density": structure["notes_per_bar"][0] <= density <= structure["notes_per_bar"][1],
    }
    return {"pass": all(checks.values()), "pass_fraction": sum(checks.values()) / len(checks),
            "checks": checks, "bars": bars, "notes_per_bar": density,
            "pitch_range": [min(pitches), max(pitches)] if pitches else None,
            "loss": sum(not v for v in checks.values()) / len(checks)}


def transcript_metrics(entries):
    attempts = []
    errors = 0
    merges = batches = 0
    for entry in entries:
        response = entry["response"]
        result = response.get("result", {})
        error = result.get("isError", False) or "error" in response
        errors += int(error)
        arguments = entry.get("arguments", {})
        steps = invocations(entry["name"], arguments)
        arguments = arguments if isinstance(arguments, dict) else {}
        name = entry["name"].split("@", 1)[0] if isinstance(entry["name"], str) else ""
        command = arguments.get("command", "")
        command = command.split("@", 1)[0] if isinstance(command, str) else ""
        batches += int(name == "batch" or name in ("exec", "query") and
                       ("batch" in arguments or command == "batch"))
        merges += sum(name == "patch.merge" for name, _ in steps)
        for name, args in steps:
            if name == "render" and "notation" in args:
                data = tool_data(result) if "content" in result or "structuredContent" in result else {}
                # A non-notation failure does not prove compilation; successful
                # scorer recompilation still selects the final submitted text.
                attempts.append({"notation": args["notation"], "compiled": not error,
                                 "error": data if error else None})
    return {"tool_calls": len(entries), "errors": errors, "patch_merges": merges,
            "batches": batches, "notation_attempts": attempts}
