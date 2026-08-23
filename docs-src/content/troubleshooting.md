---
title: Troubleshooting
description: When a machine will not answer
order: 7
---

Almost everything that goes wrong is SSH, and the fix is the same in each
case: make the plain `ssh` command work first, and shard follows.

## "no cluster.toml found"

shard looked in the current directory and in `~/.shard/`. Either run
`shard cluster init` here, or point at the file you have:

```bash
shard exec "uptime" --config ~/work/cluster.toml
```

## A machine shows as offline

Try it by hand, exactly as shard does:

```bash
ssh -o BatchMode=yes you@192.168.1.15 "uptime"
```

<table>
  <tr><th>What you see</th><th>What it means</th></tr>
  <tr><td><code>Permission denied (publickey)</code></td>
      <td>Your key is not on that machine. Run <code>ssh-copy-id you@host</code>.</td></tr>
  <tr><td><code>Connection refused</code></td>
      <td>Nothing is listening on port 22. Is the machine on? Is its SSH server running?</td></tr>
  <tr><td><code>Operation timed out</code></td>
      <td>Wrong address, or a firewall between you. Cloud machines need port 22 open to your address.</td></tr>
  <tr><td><code>Host key verification failed</code></td>
      <td>The machine's identity changed, or it is new to you. Connect once by hand and accept it.</td></tr>
  <tr><td>It asks for a password</td>
      <td>shard never types passwords. Set up a key: see <a href="/installation/">Installation</a>.</td></tr>
</table>

## It works by hand but not through shard

Check the details in `cluster.toml` — the `user`, the `port`, the `key` path.
`shard exec "uptime" --on that-machine --dry-run` prints the exact command
line being used, which usually makes the difference obvious.

If you use an SSH config file with per-host settings, note that shard passes
its own options but otherwise leaves your `~/.ssh/config` alone: a `Host`
entry with a `User` and `IdentityFile` works, and then `cluster.toml` only
needs the name.

## "command not found" on the other machine

Your login shell there is not the one your desktop session uses, so `PATH` may
be shorter than you expect. Use the full path, or set it yourself:

```bash
shard exec "/usr/local/bin/mytool --version" --on all
shard exec "export PATH=\$PATH:/usr/local/bin; mytool --version" --on all
```

## The command ran in the wrong directory

SSH starts in the login directory. Either say where to go:

```bash
shard exec "cd /srv/project && make" --on build
```

or set it once in `cluster.toml`:

```toml
workdir = "/srv/project"
```

## The output is missing

Output that never ends up in the log is usually output the program decided not
to print: many tools stay quiet when they are not talking to a terminal.
Look for a `--verbose` or `--progress` flag, or check the log file directly —
`shard logs last` shows both normal output and errors together.

## A run is stuck

Ctrl-C stops shard and takes its SSH connections down with it. Work already
started on the far machine may keep going; check with:

```bash
shard exec "ps aux | grep myjob" --on all
```

To avoid the situation, give long jobs a `--timeout`.

## The dashboard shows nothing

`shard ui` reads the same log directory the command line writes to. If they
disagree, they are using different cluster files — one of them has a
`log_dir` the other does not. `shard ui --config <file>` pins it down.
