---
title: Sharing work out
description: One list of jobs, split across your machines
order: 4
---

Running the same command everywhere is useful for maintenance. For work that
takes a long time, you usually want the opposite: one list of jobs, divided
between the machines so each does a part of it.

```bash
shard exec-script tests.sh --distribute
```

## The work list

The file is a list of commands, one per line:

```sh
# tests.sh — one line, one piece of work
pytest tests/unit -k parser
pytest tests/unit -k render
pytest tests/unit -k config
pytest tests/integration -k http
pytest tests/integration -k ssh
pytest tests/slow -k migration
```

Blank lines and lines starting with `#` are ignored. Everything else is a
piece of work that will run exactly once, on one machine.

## How the work is shared out

Nothing is decided in advance. Each machine is given a line, and when it
reports back that the line is done it is given the next one:

```bash
shard exec-script tests.sh --distribute --on cpu
```

```
tests.sh: 6 commands across 2 nodes, handed out as they finish
[laptop]       tests.sh                         ✓ completed (1m 44s)
[old-desktop]  tests.sh                         ✓ completed (1m 41s)

  laptop           1 worker  · 4 items
  old-desktop      1 worker  · 2 items

6/6 items completed in 1m 44s
```

The counts at the end are what each machine actually did, not what it was
promised. A machine that turns out to be twice as fast simply asks for twice
as much, and none of them sit idle while another finishes a long line. You do
not have to guess capacity correctly in `cluster.toml` for this to work.

While it runs, a single line at the bottom of the terminal keeps count:

```
  17/60 items · about 2m 30s left
```

## When a line fails

The other lines carry on. shard counts the failures, reports the run as
failed, and exits with status 1:

```
  laptop           1 worker  · 4 items
  old-desktop      1 worker  · 2 items
  2 of 6 items failed

4/6 items completed in 1m 44s, 2 failed
```

For a test suite that is usually what you want: one broken test should not
hide the state of the others.

## --static, when you want the old certainty

`--static` decides the shares before anything runs, in proportion to the `cpu`
you gave each machine, and sends each machine its share as one script:

```bash
shard exec-script tests.sh --distribute --static
```

```
tests.sh: 6 commands across 2 nodes
  laptop           lines 1-4 (4)
  old-desktop      lines 5-6 (2)
```

Two differences worth knowing:

- The split is fixed, so a machine that turns out to be slow holds up the run.
- Each machine's share runs with `set -e`: its first failure skips the rest of
  *its* lines.

It is the right choice when you want the run reproducible — the same machine
gets the same lines every time — or when the machines cannot talk back for
some reason. Otherwise the default balances better.

## The other shape of this

`--distribute` is for a list of different commands. When it is one command
repeated with a changing number — a thousand seeds, three hundred frames —
[`shard map`](/guide/many-runs/) says it in one line:

```bash
shard map "./simulate --seed {i}" --count 1000
```

## A rule of thumb

Distributing helps when the work is longer than the connection overhead. A
few seconds per line is plenty. Splitting six commands that each take 20
milliseconds across four machines will be slower than running them here, and
that is fine — the tool is for the other case.
