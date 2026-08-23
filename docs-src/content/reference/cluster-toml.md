---
title: cluster.toml
description: Every configuration key
order: 2
---

The file has one `[cluster]` block and one `[[nodes]]` block per machine. The
double brackets matter: they are how TOML writes a list.

## [cluster]

<table>
  <tr><th>Key</th><th>Default</th><th>Meaning</th></tr>
  <tr><td><code>name</code></td><td>the machine's hostname</td>
      <td>What this cluster is called. Shown in listings and the dashboard.</td></tr>
  <tr><td><code>log_dir</code></td><td><code>~/.shard/logs</code></td>
      <td>Where run output is kept. <code>~</code> is expanded.</td></tr>
</table>

## [[nodes]]

<table>
  <tr><th>Key</th><th>Default</th><th>Meaning</th></tr>
  <tr><td><code>name</code></td><td>the host</td>
      <td>What you type to select this machine.</td></tr>
  <tr><td><code>host</code></td><td>the name</td>
      <td>Address or hostname. <code>localhost</code> runs without SSH.</td></tr>
  <tr><td><code>user</code></td><td>your username</td>
      <td>SSH user.</td></tr>
  <tr><td><code>key</code></td><td>your default key</td>
      <td>SSH identity file. <code>~</code> is expanded.</td></tr>
  <tr><td><code>port</code></td><td>22</td><td>SSH port.</td></tr>
  <tr><td><code>tags</code></td><td>none</td>
      <td>List of labels, for selecting groups: <code>tags = ["cloud", "gpu"]</code>.</td></tr>
  <tr><td><code>cpu</code></td><td>1</td>
      <td>Cores. Decides this machine's share when work is distributed.</td></tr>
  <tr><td><code>ram_gb</code></td><td>none</td>
      <td>Memory, for your own reference. Shown in listings.</td></tr>
  <tr><td><code>workdir</code></td><td>the login directory</td>
      <td>Directory to change into before running anything.</td></tr>
  <tr><td><code>local</code></td><td>false</td>
      <td><code>true</code> runs commands here, without SSH. Implied by <code>localhost</code>.</td></tr>
</table>

## A file using all of it

```toml
[cluster]
name    = "home-lab"
log_dir = "~/.shard/logs"

[[nodes]]
name    = "laptop"
host    = "localhost"
tags    = ["local", "fast"]
cpu     = 8
ram_gb  = 16

[[nodes]]
name    = "build-box"
host    = "192.168.1.15"
user    = "yavuz"
port    = 2222
workdir = "/srv/build"
tags    = ["cpu", "build"]
cpu     = 4
ram_gb  = 8

[[nodes]]
name   = "ec2-worker"
host   = "ec2-13-58-0-1.compute.amazonaws.com"
user   = "ubuntu"
key    = "~/.ssh/aws-key.pem"
tags   = ["cloud", "cpu"]
cpu    = 4
ram_gb = 8
```

## What is understood

The reader covers the parts of TOML this file needs: comments, `[table]` and
`[[array of tables]]` headers, strings, whole numbers, `true`/`false`, and
lists of strings. Nested tables, dates and floating point numbers are not
used, and a key it does not recognise is ignored rather than refused — so a
note to yourself in the file does no harm.

A syntax error stops the command and names the line, rather than half-running
on a misread cluster.
