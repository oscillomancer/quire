# SPDX-License-Identifier: GPL-3.0-or-later
"""Scoring over authoritative compiler/analysis output."""

import bisect
import math

from relay import invocations
from common import tool_data


def ticks_per_bar(ppq, meter):
    numerator, denominator = map(int, meter.split("/"))
    return ppq * numerator * 4 / denominator


def attacks_per_bar(midi, bars):
    """Each bar's note starts, every note of a chord counted, by its own meter."""
    meters = midi.get("bar_meters") or [midi["meter"]] * math.ceil(bars)
    starts, at = [], 0
    for meter in meters:
        starts.append(at)
        at += ticks_per_bar(midi["ppq"], meter)
    counts = [0] * len(starts)
    for note in midi["notes"]:
        bar = bisect.bisect_right(starts, note["start"]) - 1
        if 0 <= bar < len(counts):
            counts[bar] += 1
    return counts


def notation_score(midi, structure, attempts):
    bars = midi["length_ticks"] / ticks_per_bar(midi["ppq"], midi["meter"])
    pitches = [n["pitch"] for n in midi["notes"]]
    per_bar = attacks_per_bar(midi, bars)
    low, high = structure["notes_per_bar"]
    checks = {
        "first_attempt": bool(attempts) and attempts[0]["compiled"],
        "bars": bars == structure["bars"],
        "meter": midi["meter"] == structure["meter"] and all(
            m == structure["meter"] for m in midi.get("bar_meters", [])),
        "pitch_range": bool(pitches) and min(pitches) >= structure["pitch_range"][0]
                       and max(pitches) <= structure["pitch_range"][1],
        "density": bool(per_bar) and all(low <= count <= high for count in per_bar),
    }
    return {"pass": all(checks.values()), "pass_fraction": sum(checks.values()) / len(checks),
            "checks": checks, "bars": bars, "notes_per_bar": per_bar,
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
