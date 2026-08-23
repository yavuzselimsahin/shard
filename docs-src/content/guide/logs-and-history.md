---
title: Logs and history
description: Where the output goes, and how to get it back
order: 9
---

Every run is kept on disk. Nothing is thrown away when the command finishes,
and nothing needs a database.

## The last run

```bash
shard logs last
```

```
20260822-211013-1490  completed  make -j4

=== laptop ===
gcc -O2 -c src/main.c
gcc -O2 -c src/parser.c
…

=== old-desktop ===
gcc -O2 -c src/render.c
…
```

Output from one machine only:

```bash
shard logs last --node old-desktop
```

## Earlier runs

```bash
shard history
```

```
TASK                   STATUS      COMMAND                      DURATION  STARTED
20260822-211013-1496   failed      ./deploy.sh                    10.03s  2026-08-22 21:10:13
20260822-211013-1490   completed   make -j4                        4.01s  2026-08-22 21:10:13
20260822-211002-1479   completed   uname -sm                       1.01s  2026-08-22 21:10:02
```

The first column is the run's id. Pass it to `shard logs`:

```bash
shard logs 20260822-211013-1490
```

`shard history -n 50` shows more; the default is 20.

## What is running now

```bash
shard status
```

Runs that are still going are listed with the machines they are using. A run
whose command was interrupted — you closed the terminal, the laptop slept —
shows as `interrupted` rather than pretending to still be alive: shard checks
whether the process is really there.

## The files themselves

Everything lives under `~/.shard/logs`, one directory per run:

```
~/.shard/logs/20260822-211013-1490/
├── task.json          what ran, where, how long, and how it ended
├── laptop.log         everything that machine printed
└── old-desktop.log
```

They are plain files. `grep`, `tail -f` and your editor all work on them:

```bash
tail -f ~/.shard/logs/20260822-211013-1490/laptop.log
grep -c FAILED ~/.shard/logs/*/laptop.log
```

Output is written as it arrives, so following a log while the run is going is
a perfectly good way to watch it.

## Housekeeping

Nothing expires on its own. To clear old runs:

```bash
rm -rf ~/.shard/logs/2026080*
```

To keep logs somewhere else — a bigger disk, a shared folder — set `log_dir`
in `cluster.toml`:

```toml
[cluster]
log_dir = "/mnt/data/shard-logs"
```

To skip logging entirely for one command, add `--no-log`. The summary is
still printed; nothing is written to disk.
