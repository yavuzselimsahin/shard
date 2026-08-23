---
title: Quick start
description: From nothing to a command running everywhere
order: 3
---

This takes about five minutes. You will end with a cluster file, a health
check, and one command running on every machine in it.

## 1. Create a cluster file

In the directory where you work:

```bash
shard cluster init
```

That writes a `cluster.toml` containing one machine: the one you are sitting
at. Everything works with just that machine, which makes it a safe place to
try things.

## 2. Add your other machines

Open `cluster.toml` and add a block per machine:

```toml
[[nodes]]
name   = "old-desktop"
host   = "192.168.1.15"
user   = "yavuz"
tags   = ["cpu"]
cpu    = 2
```

`name` is what you will type. `host` is the address. `user` is the SSH user,
if it differs from yours. `cpu` tells shard how much work this machine can
take relative to the others. `tags` let you address several machines at once.

There is an interactive version if you prefer being asked:

```bash
shard cluster add-node
```

## 3. Check that they answer

```bash
shard cluster health
```

```
● laptop           online   8 CPU  16384 MB  ▓▓░░░░░░░░  18% cpu  ▓▓▓▓░░░░░░  41% ram  2 ms
● old-desktop      online   2 CPU   3900 MB  ░░░░░░░░░░   4% cpu  ▓▓░░░░░░░░  22% ram  31 ms
○ ec2-worker       offline  ssh: connect to host … : Connection refused

2/3 online
```

A machine that shows as offline is one you cannot SSH into without a password.
[Troubleshooting](/troubleshooting/) walks through the usual causes.

## 4. Run something

```bash
shard exec "uname -sm" --on all
```

```
[laptop]       uname -sm                        ✓ completed (0.12s)
[old-desktop]  uname -sm                        ✓ completed (0.44s)

2/2 completed in 0.44s
Total machine time: 0.56s (21% faster than one after another)
Logs: shard logs 20260822-211013-1490
```

The commands run at the same time, not one after the other. The run finishes
when the slowest machine finishes.

## 5. Read the output

The summary line tells you what happened; the output itself is in the logs:

```bash
shard logs last
```

## 6. Watch it in a browser

```bash
shard ui
```

Open <http://localhost:8787>. You get the machines, what they are doing, and
the output of every run — see [The web dashboard](/guide/web-dashboard/).

## Where next

- [Running commands](/guide/running-commands/) — choosing machines, live
  output, timeouts.
- [Sharing work out](/guide/sharing-work/) — splitting a long list of jobs
  across machines instead of repeating it on each.
- [Running one thing many times](/guide/many-runs/) — one command, a thousand
  different numbers.
- [Named jobs](/guide/tasks-file/) — writing the work down in `tasks.toml` so
  it becomes `shard run <name>`.
