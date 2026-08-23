#!/usr/bin/env python3
"""Hash a batch of passwords with PBKDF2 — a deliberately heavy, real workload.

Password hashing is supposed to be slow; that is the point of it. Benchmarking
how many hashes a machine manages, or pre-hashing a batch of credentials, is a
genuine reason to spread work over several machines. The --rounds knob turns
the load up directly. Standard library only.

    python3 pbkdf2_bench.py --count 200 --rounds 400000 --tag batch-3
"""
import argparse, hashlib, os, sys, time

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=200, help="passwords in this batch")
    ap.add_argument("--rounds", type=int, default=400_000, help="PBKDF2 iterations; the load knob")
    ap.add_argument("--tag", default="batch", help="label for this batch, e.g. a shard index")
    args = ap.parse_args()

    start = time.time()
    last = ""
    for i in range(args.count):
        password = f"{args.tag}-user-{i}".encode()
        salt = os.urandom(16)
        dk = hashlib.pbkdf2_hmac("sha256", password, salt, args.rounds)
        last = dk.hex()

    took = time.time() - start
    rate = args.count / took if took else 0
    print(f"HASH tag={args.tag} count={args.count} rounds={args.rounds} "
          f"secs={took:.2f} rate={rate:.1f}/s sample={last[:16]}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
