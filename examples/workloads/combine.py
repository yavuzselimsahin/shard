#!/usr/bin/env python3
"""Average the pi estimates a montecarlo_pi run left in its logs.

    shard logs last | python3 combine.py
"""
import math, re, sys

vals, samples = [], 0
for line in sys.stdin:
    m = re.search(r"estimate=([0-9.]+) .*samples=(\d+)", line) or \
        re.search(r"samples=(\d+) estimate=([0-9.]+)", line)
    if not m:
        continue
    if "estimate=" in line and line.index("estimate=") < line.index("samples="):
        est, n = float(m.group(1)), int(m.group(2))
    else:
        n, est = int(m.group(1)), float(m.group(2))
    vals.append(est)
    samples += n

if not vals:
    print("no pi estimates found on stdin", file=sys.stderr)
    sys.exit(1)

mean = sum(vals) / len(vals)
print(f"{len(vals)} estimates, {samples:,} samples total")
print(f"pi ~= {mean:.6f}   (error {abs(mean - math.pi):.6f})")
