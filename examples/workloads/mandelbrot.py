#!/usr/bin/env python3
"""Render one frame of a Mandelbrot zoom — the "render farm" workload.

Each frame is an independent, heavy computation, which is exactly what a `map`
spreads well: give it the frame count and every machine renders its share into
its own output directory. A home lab rendering a zoom is the real version of
this. Standard library only; writes a small PGM so you can open the result.

    python3 mandelbrot.py --frame 42 --frames 300 --size 512 --out frames
"""
import argparse, math, os, sys, time

def render(frame: int, frames: int, size: int, max_iter: int) -> bytearray:
    # Zoom towards a point on the edge of the set; deeper frames need more
    # iterations to stay sharp, so later frames are genuinely heavier.
    cx, cy = -0.743643887037151, 0.13182590420533
    scale = 3.0 * (0.92 ** frame)
    iters = max_iter + frame * 4

    buf = bytearray(size * size)
    half = size / 2
    for py in range(size):
        y0 = cy + (py - half) / half * scale
        row = py * size
        for px in range(size):
            x0 = cx + (px - half) / half * scale
            x = y = 0.0
            i = 0
            while x * x + y * y <= 4.0 and i < iters:
                x, y = x * x - y * y + x0, 2.0 * x * y + y0
                i += 1
            buf[row + px] = 0 if i >= iters else 255 - int(255 * i / iters)
    return buf

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frame", type=int, required=True)
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--size", type=int, default=512, help="pixels per side; the load knob")
    ap.add_argument("--max-iter", type=int, default=200)
    ap.add_argument("--out", default="frames")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    start = time.time()
    buf = render(args.frame, args.frames, args.size, args.max_iter)

    path = os.path.join(args.out, f"frame_{args.frame:04d}.pgm")
    with open(path, "wb") as f:
        f.write(f"P5\n{args.size} {args.size}\n255\n".encode())
        f.write(buf)

    print(f"FRAME {args.frame:04d} -> {path} ({args.size}x{args.size}, "
          f"{time.time() - start:.2f}s)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
