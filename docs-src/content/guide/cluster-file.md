---
title: The cluster file
description: Describing your machines in cluster.toml
order: 1
---

`cluster.toml` is the only configuration shard has. It lists the machines you
can use and a few facts about each one.

shard looks for it in two places, in this order:

1. `cluster.toml` in the directory you are in,
2. `~/.shard/cluster.toml`.

The first one lets a project carry its own set of machines. The second is your
personal default, used wherever you happen to be. To point at a different file
for one command, use `--config`:

```bash
shard exec "uptime" --config ~/work/build-farm.toml
```

## A complete example

```toml
[cluster]
name    = "home-lab"
log_dir = "~/.shard/logs"

[[nodes]]
name = "laptop"
host = "localhost"
tags = ["local", "fast"]
cpu  = 8
ram_gb = 16

[[nodes]]
name   = "old-desktop"
host   = "192.168.1.15"
user   = "yavuz"
tags   = ["cpu"]
cpu    = 2
ram_gb = 4

[[nodes]]
name   = "ec2-worker"
host   = "ec2-13-58-0-1.compute.amazonaws.com"
user   = "ubuntu"
key    = "~/.ssh/aws-key.pem"
tags   = ["cloud", "cpu"]
cpu    = 4
ram_gb = 8
```

A machine at home and a machine in a data centre are written the same way. The
only difference is the address, and sometimes a key file.

## What a machine can say about itself

<table>
  <tr><th>Key</th><th>Meaning</th></tr>
  <tr><td><code>name</code></td>
      <td>What you type to select this machine. Required in practice.</td></tr>
  <tr><td><code>host</code></td>
      <td>Address or hostname. <code>localhost</code> means this machine.</td></tr>
  <tr><td><code>user</code></td>
      <td>SSH user. Left out, SSH uses your own username.</td></tr>
  <tr><td><code>key</code></td>
      <td>SSH key file, such as <code>~/.ssh/aws-key.pem</code>.</td></tr>
  <tr><td><code>port</code></td>
      <td>SSH port, if it is not 22.</td></tr>
  <tr><td><code>tags</code></td>
      <td>Labels you invent, for addressing groups of machines.</td></tr>
  <tr><td><code>cpu</code></td>
      <td>Core count. Used to decide how much work this machine takes.</td></tr>
  <tr><td><code>ram_gb</code></td>
      <td>Memory, shown in listings. Not used for scheduling yet.</td></tr>
  <tr><td><code>workdir</code></td>
      <td>Directory to change into before running anything.</td></tr>
  <tr><td><code>local</code></td>
      <td><code>true</code> to run without SSH. Implied by <code>localhost</code>.</td></tr>
</table>

Everything except `name` and `host` is optional.

## This machine counts too

A node whose host is `localhost` runs commands directly, without SSH. That is
worth having: your own machine is usually the fastest one you own, and
including it means a two-machine cluster needs no SSH setup at all to try.

## Tags

Tags are yours to invent. Common ones are the kind of machine (`gpu`, `cpu`),
where it is (`home`, `cloud`), or what it is for (`build`, `test`).

```toml
tags = ["cloud", "cpu"]
```

Then address the group:

```bash
shard exec "apt-get update" --on cloud
```

A machine with several tags answers to each of them.

## Where logs go

`log_dir` under `[cluster]` sets where output is kept. The default,
`~/.shard/logs`, suits most people. Point it at a shared disk if you want runs
from several projects in one place.

## Editing the file

`shard cluster add-node` asks a few questions and appends a block. Removing
one is a line edit rather than a rewrite, so the comments you left in the file
survive:

```bash
shard cluster remove old-desktop
```

The file is plain text and yours to edit by hand at any time — shard reads it
fresh on every command.
