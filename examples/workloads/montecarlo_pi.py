#!/usr/bin/env python3
"""Estimate pi by throwing darts at a square — one shard item, one batch.

Embarrassingly parallel: every item uses its own seed, so a thousand of them
running on a dozen machines never touch each other. The estimates are averaged
at the end (see combine.py) and converge on pi. Pure CPU, standard library
only, so it runs anywhere python does.

    python3 montecarlo_pi.py --seed 7 --samples 20000000
"""
import argparse, random, sys, time

def estimate(seed: int, samples: int) -> float:
    rng = random.Random(seed)
    inside = 0
    # A tight loop kept in local variables: this is the part meant to burn CPU.
    rand = rng.random
    for _ in range(samples):
        x = rand()
        y = rand()
        if x * x + y * y <= 1.0:
            inside += 1
    return 4.0 * inside / samples

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--samples", type=int, default=20_000_000,
                    help="darts to throw; this is the load knob")
    args = ap.parse_args()

    start = time.time()
    pi = estimate(args.seed, args.samples)
    took = time.time() - start

    # One machine-readable line, so combine.py can average many of them.
    print(f"PI seed={args.seed} samples={args.samples} estimate={pi:.6f} secs={took:.2f}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
