---
title: Running commands
description: shard exec, and choosing what runs where
order: 3
---

```bash
shard exec "<command>" [--on <where>]
```

The command is run by the shell on each machine, so pipes, redirects and
`&&` all work — quote the whole thing so your own shell leaves it alone.

```bash
shard exec "df -h / | tail -1" --on all
```

## Choosing machines

`--on` takes a name, a tag, several of either separated by commas, or `all`:

```bash
shard exec "uptime" --on all                 # everything in the file
shard exec "uptime" --on cloud               # every machine tagged cloud
shard exec "uptime" --on laptop              # one machine
shard exec "uptime" --on laptop,ec2-worker   # two of them
```

Left out, `--on all` is assumed. If nothing matches, shard tells you and does
nothing — a typo in a tag never silently runs on everything.

## Reading the result

```
[laptop]       make -j4                         ✓ completed (2m 14s)
[old-desktop]  make -j4                         ✓ completed (5m 03s)
[ec2-worker]   make -j4                         ✗ failed (exit 2) (1m 47s)

2/3 succeeded in 5m 03s
Total machine time: 9m 04s (44% faster than one after another)
Logs: shard logs 20260822-211013-1490
```

One line per machine, printed the moment that machine finishes. Underneath:
how many succeeded, how long the whole thing took, and how much time it would
have taken one machine after another.

`shard exec` exits with status 1 if any machine failed, so it drops into a
script or a CI job without further work.

## Watching output as it arrives

By default the output goes to the log files and the terminal stays quiet. To
see it live, with each line labelled by the machine it came from:

```bash
shard exec "make -j4" --on build --stream
```

```
[laptop]       gcc -O2 -c src/main.c
[old-desktop]  gcc -O2 -c src/parser.c
[laptop]       gcc -O2 -c src/render.c
```

With more than two or three talkative machines this gets busy. The logs are
still written either way, so `--stream` is about watching, not about keeping.

## Not waiting forever

A machine that hangs holds up the whole run. Give it a limit:

```bash
shard exec "./flaky-job.sh" --timeout 300
```

After 300 seconds that machine's job is killed — along with anything it
started — and reported as timed out. The other machines carry on.

## Not running everything at once

By default every selected machine starts immediately. To keep the number of
simultaneous jobs down, for instance because they all pull from the same
network share:

```bash
shard exec "./sync.sh" --on all --jobs 2
```

## Trying again

Some failures are not about your command: a machine that was asleep, a network
that blinked, a package server that timed out. `--retry` gives a failed job
another go:

```bash
shard exec "./flaky-sync.sh" --on all --retry 2
```

```
[old-desktop]  ./flaky-sync.sh   ✗ failed (exit 1) (0.40s) ↺ trying again
[old-desktop]  ./flaky-sync.sh   ✓ completed (12.10s)
```

Both attempts are kept in the log, one after the other, so you can see what
went wrong the first time. Nothing is retried unless you ask: a command that
half-finished before failing may not be safe to run twice, and shard cannot
know that.

## Looking before leaping

`--dry-run` prints the exact command line shard would run for each machine,
and runs nothing:

```bash
shard exec "rm -rf build" --on all --dry-run
```

```
[laptop] /bin/sh -c rm -rf build
[old-desktop] ssh -o BatchMode=yes … yavuz@192.168.1.15 rm -rf build
```

This is worth doing before anything destructive. There is no undo across
twelve machines.

## Running a script instead of a command

If the work is longer than one line, put it in a file:

```bash
shard exec-script setup.sh --on all
```

The file is piped to each machine's shell over the same SSH connection.
Nothing is copied to disk on the far side and nothing is left behind.

That works because the shell reads the whole script from the connection. A
command that runs a *file* — `python3 sim.py` — needs that file on the machine
first; [`--with`](/guide/shipping-code/) sends it along.

To split the work between machines rather than repeat it on each, see
[Sharing work out](/guide/sharing-work/).
