---
title: Example workloads
description: Three heavy jobs to try the tool on
order: 5
---

`echo hello` shows that shard reaches your machines. To see what it is *for*,
you want work that actually keeps them busy for a minute or two — that is where
the speedup, the live progress and the dashboards mean something.

These three come with the source, under [`examples/workloads/`][dir]. Each is a
small standard-library Python script — nothing to install — and each shows off
a different strategy. They all run on one machine; point `cluster.toml` at more
and the same commands spread across them. The `--with` on each command
[ships the script](/guide/shipping-code/) to the machines that do not have it.

[dir]: https://github.com/yavuzselimsahin/shard/tree/main/examples/workloads

## 1. Monte Carlo π — a `map`

Throw darts at a square and count how many land in the circle. Every trial is
independent, which is exactly what [`map`](/guide/many-runs/) spreads well:

```bash
shard map "python3 montecarlo_pi.py --seed {i} --samples 20000000" \
  --count 200 --on cpu --retry 1 --with montecarlo_pi.py
```

The estimates average out to π — verify it straight from the logs:

```bash
shard logs last | python3 combine.py
# 200 estimates, 4,000,000,000 samples total
# pi ~= 3.141582   (error 0.000011)
```

Turn the load up with `--samples`.

## 2. Mandelbrot zoom — a render farm

Render hundreds of frames of a zoom, each an independent heavy picture — the
home-lab render farm, handed out frame by frame:

```bash
shard map "python3 mandelbrot.py --frame {i} --frames 300 --size 512 --out frames" \
  --count 300 --on cpu --with mandelbrot.py
```

Each machine writes its share into its own `frames/` directory. Count them:

```bash
shard exec "ls frames | wc -l" --on cpu
```

Later frames zoom deeper and take longer, so this is a good way to watch
dynamic dispatch give the free machines more to do. Turn the load up with
`--size`.

## 3. PBKDF2 batches — uneven on purpose

Password hashing is meant to be slow, which makes it a real reason to spread
the work out. This work list is **deliberately lumpy** — every sixth batch is
ten times heavier — so you can see [`--distribute`](/guide/sharing-work/) hand
the light batches out fast and keep every machine busy:

```bash
sh make_worklist.sh > worklist.sh
shard exec-script worklist.sh --distribute --on cpu --with pbkdf2_bench.py
```

The per-machine counts at the end come out uneven, because the machines that
drew heavy batches took fewer of them:

```
  laptop        1 worker · 5 items
  old-desktop   1 worker · 3 items      <- drew two heavy batches
  ec2-worker    1 worker · 6 items
```

Run it again with `--static` to feel the difference: fixed shares finish only
when the unluckiest machine does.

## Watching it happen

While any of these runs, in another terminal:

```bash
shard tui           # live, in the terminal
shard ui            # live, in a browser
```

Both fill a progress bar, show the per-machine item counts, and tail the log
of whichever worker you pick. The full walkthrough, with load knobs and
`tasks.toml` entries for all three, is in the workloads
[README][dir].
