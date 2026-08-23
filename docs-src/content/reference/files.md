---
title: Files and exit codes
description: What shard writes, and what it returns
order: 4
---

## What shard writes

<table>
  <tr><th>Path</th><th>What it is</th></tr>
  <tr><td><code>./cluster.toml</code></td>
      <td>The cluster for this directory. Written by <code>shard cluster init</code>.</td></tr>
  <tr><td><code>~/.shard/cluster.toml</code></td>
      <td>Your default cluster, used when the directory has none.</td></tr>
  <tr><td><code>~/.shard/logs/&lt;run-id&gt;/</code></td>
      <td>One directory per run.</td></tr>
  <tr><td><code>~/.shard/logs/&lt;run-id&gt;/task.json</code></td>
      <td>What ran, on which machines, how long it took, how it ended.</td></tr>
  <tr><td><code>~/.shard/logs/&lt;run-id&gt;/&lt;machine&gt;.log</code></td>
      <td>Everything that machine printed, output and errors together.</td></tr>
  <tr><td><code>~/.shard/state/&lt;cluster&gt;-health.json</code></td>
      <td>The last health check, so the dashboard can show it immediately.</td></tr>
</table>

Nothing is written on the machines you run work on.

A run id looks like `20260822-211013-1490`: the date, the time, and the
process that started it.

## Exit codes

<table>
  <tr><th>Code</th><th>Meaning</th></tr>
  <tr><td><code>0</code></td><td>Everything succeeded.</td></tr>
  <tr><td><code>1</code></td><td>At least one machine failed, or the command could not run at all.</td></tr>
</table>

Per-machine exit codes are reported in the summary and stored in `task.json`.
Two are shard's own rather than your command's:

<table>
  <tr><th>Code</th><th>Meaning</th></tr>
  <tr><td><code>124</code></td><td>The job hit <code>--timeout</code> and was killed.</td></tr>
  <tr><td><code>255</code></td><td>SSH could not connect. The log holds the reason.</td></tr>
</table>

## How a command reaches a machine

For a remote machine:

```
ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
    -o ConnectTimeout=10 -o LogLevel=ERROR \
    [-i key] [-p port] user@host <your command>
```

For a local one:

```
/bin/sh -c <your command>
```

`--dry-run` prints exactly this, for every selected machine, without running
anything.

`BatchMode=yes` is why a machine that wants a password is reported as
unreachable instead of stopping the run to ask.
