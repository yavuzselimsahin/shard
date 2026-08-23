---
title: Shipping your code
description: Running a script on machines that have never seen it
order: 2
---

The whole idea is to run *your* code on other machines — machines that borrow
you their CPU without knowing anything about the job. So the code has to travel
with the work.

By default it does not. `shard exec "python3 sim.py"` runs `python3 sim.py` on
each machine, and if `sim.py` is not already there, it fails. `--with` fixes
that: it sends the files along before the job runs.

```bash
shard map "python3 sim.py --seed {i}" --count 1000 --with sim.py
```

Now every machine gets a copy of `sim.py`, runs its share, and the copy is
removed afterwards. Nothing was installed and nothing is left behind.

## What you can send

Repeat `--with` for more than one file, and name a directory to send a whole
tree:

```bash
shard map "python3 run.py --case {i}" --count 500 \
  --with run.py --with helpers.py --with data/
```

Paths are relative to where you are standing. On the far machine they keep the
same shape: `--with data/` becomes a `data/` next to your command, so
`python3 run.py` finds `data/` exactly where it expects it.

## Where it runs

The files land in a fresh per-run directory on each machine, and the command
runs there. When the run ends the directory is deleted. To keep it — to poke
around after a failure — add `--keep`:

```bash
shard exec "python3 build.py" --on cpu --with build.py --keep
```

## What does not travel back

`--with` sends code *to* the machines. Whatever the job writes stays on the
machine that produced it, in that per-run directory (or wherever your command
put it). Three ways to get results back:

- **Print them.** Anything the job writes to output is in the logs:
  `shard logs last`. The Monte Carlo example averages its answer straight out
  of the logs this way.
- **In a pipeline**, name the files with `produces` and the next step receives
  them — see [Steps and pipelines](/guide/pipelines/).
- **Copy them yourself** at the end of the job, with an `scp` or an upload to
  wherever you keep results.

## This machine is not shipped to

A `localhost` node already has your files in the working directory, so nothing
is sent to it — it just runs where you are. `--with` only touches the machines
that actually need the code.

## Not just Python

Nothing here is language-specific. Ship a shell script, a compiled binary, a
jar — anything the far machine can run:

```bash
shard exec "./render --frame 1" --on gpu --with render --with scene.blend
```

The one thing shard assumes is that the machine can *run* what you send: a
binary built for your laptop will not run on a different kind of machine. Code
that the far machine builds or interprets — a script, or source it compiles
first — travels without that worry.
