---
title: The web dashboard
description: Watching the cluster in a browser
order: 10
---

```bash
shard ui
```

```
shard ui — home-lab (3 nodes)
Open http://localhost:8787  ·  Ctrl-C to stop
```

The dashboard is part of the same binary. There is nothing to install, no
`npm`, and no server to keep running — start it when you want it, stop it with
Ctrl-C.

## What it shows

**Nodes.** Every machine in `cluster.toml`, with its address, tags and — after
a health check — how busy it is. The **Check health** button runs the same
check `shard cluster health` does and updates the bars.

**Running.** Anything happening right now, machine by machine, refreshed every
two seconds. Runs you started in a terminal appear here: the dashboard reads
the same log directory.

A run made of items — a `map`, or a distributed script — shows a real
progress bar: how many of the thousand are done, how many failed, and roughly
how long is left.

**Recent runs.** The last 25 runs, newest first. Click one to see its
machines; click a machine to read its output, which keeps updating while the
run is going.

## It only reads

The dashboard cannot start work. It shows what is happening and what has
happened; you start runs from the terminal. That keeps the browser page
harmless — a stray click cannot launch anything on twelve machines.

The one thing it can do is run a health check, because that is a read too.

## Who can reach it

The server listens on `localhost` only. Other machines on your network cannot
connect to it, and neither can anything on the internet. That is deliberate:
the page shows your machines and your command output.

To watch it from another computer, forward the port over SSH rather than
opening it up:

```bash
ssh -L 8787:localhost:8787 you@the-machine-running-shard
```

Then open <http://localhost:8787> on your own computer.

## Or without a browser

`shard tui` shows the same thing in the terminal, which is usually easier over
SSH — see [The terminal dashboard](/guide/terminal-dashboard/).

## A different port

```bash
shard ui 9000
```

## While it is running

The dashboard reads `cluster.toml` when it starts. If you add a machine to the
file, stop it with Ctrl-C and start it again to see the change.
