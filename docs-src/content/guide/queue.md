---
title: Queueing jobs
description: More jobs than patience — line them up and walk away
order: 8
---

`shard run` and `shard map` run one job and hand the terminal back. When you
have several jobs to get through — a few maps, a build, an overnight batch —
you do not want to start each one by hand as the last finishes. The queue does
that for you.

```bash
shard queue add -- map "python3 sim.py --seed {i}" --count 1000 --on cpu --with sim.py
shard queue add -- run nightly-report
shard queue run
```

`queue run` works through the jobs one at a time and stops when the queue is
empty. One at a time is deliberate: each job already spreads itself across the
whole cluster, so running two at once would just make them fight for the same
machines.

## Saying what matters more

A job's priority decides where it lands in the line. Higher runs first; the
default is 0.

```bash
shard queue add --priority 10 --name urgent -- run deploy
shard queue add --priority 1  --name nightly -- run backup
```

Within the same priority it is first-in, first-out. `--name` gives a job a
label so the listing and the run are easy to read.

```bash
shard queue list
```

```
3 jobs waiting, in the order they will run:

PRI           ADDED               COMMAND
10   urgent   2026-08-23 18:48:41 run deploy
1    nightly  2026-08-23 21:00:12 run backup
0             2026-08-23 21:03:55 map python3 sim.py --seed {i} --count 1000 …
```

The order is recomputed every time a job finishes, so a high-priority job you
add from another terminal while the queue is running jumps ahead of the
lower-priority ones still waiting — it just does not interrupt the job already
in progress.

## It survives everything

Each queued job is a small file under `~/.shard/queue`. The queue is still
there after you close the terminal, and a job is removed only once it has
actually run — so if the machine you are on reboots mid-queue, `shard queue
run` picks up where it left off.

Each job also remembers the directory it was added in, and runs there. Relative
paths and `--with` files resolve the way they did when you queued the job, no
matter where you run the queue from.

## Stopping

`Ctrl-C` stops the queue after the job in progress finishes, rather than
killing it midway. What has not run yet stays queued for next time.

To stop the moment something fails — useful when later jobs depend on earlier
ones — use `--stop-on-fail`:

```bash
shard queue run --stop-on-fail
```

Without it, a failed job is reported and the queue carries on; the count at the
end says how many failed.

## Managing the queue

```bash
shard queue remove <id>     # drop one job (the id is in `queue list`)
shard queue clear           # empty the whole queue
```

## What can be queued

Anything you would type after `shard` except the queue itself: `exec`,
`exec-script`, `map`, `pipeline`, `run`. Put the whole command after `--`:

```bash
shard queue add --priority 5 -- exec "make -j4" --on build --with Makefile
shard queue add -- pipeline --step "./build.sh" --on fast --produces out.tar \
                            --step "./ship.sh out.tar" --on cloud
```
