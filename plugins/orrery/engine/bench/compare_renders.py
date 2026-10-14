#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare --dump-guide captures: reference + all guide JSON examples, 8 s each.

Usage: python3 plugins/orrery/engine/bench/compare_renders.py BASE.raw NEW.raw
No third-party Python dependencies. Fails on nonfinite audio, silence, size
mismatch, or a residual above -80 dB relative to the baseline's stereo RMS.
"""

import array
import math
from pathlib import Path
import sys


def main():
    guide = Path(__file__).resolve().parents[4] / "skill/guides/sound-design.md"
    count = 1 + guide.read_text().count("```json\n")
    samples = 3000 * 128 * 2
    failed = False
    with open(sys.argv[1], "rb") as base, open(sys.argv[2], "rb") as new:
        for i in range(count):
            a, b = array.array("f"), array.array("f")
            a.fromfile(base, samples)
            b.fromfile(new, samples)
            if not all(math.isfinite(x) for x in a) or not all(math.isfinite(x) for x in b):
                raise ValueError(f"nonfinite audio in patch {i}")
            signal = math.fsum(x * x for x in a)
            error = math.fsum((x - y) ** 2 for x, y in zip(a, b))
            if signal == 0:
                raise ValueError(f"silent baseline in patch {i}")
            db = 10 * math.log10(error / signal) if error else -math.inf
            name = "reference" if i == 0 else f"guide JSON {i:02}"
            print(f"{name:16} residual {db:8.2f} dB; RMS {math.sqrt(signal / samples):.6f}")
            failed |= db > -80
        if base.read(1) or new.read(1):
            raise ValueError("unexpected capture size")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
