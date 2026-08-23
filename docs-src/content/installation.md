---
title: Installation
description: Building shard and putting it on your PATH
order: 2
---

shard is a single binary. Building it needs a C compiler and `make`, both of
which macOS and every Linux distribution already have or can install in one
step.

## Build from source

```bash
git clone https://github.com/yavuzselimsahin/shard.git
cd shard
make
```

The result is a file called `shard` in that directory. Copy it somewhere on
your `PATH`:

```bash
sudo make install          # puts it in /usr/local/bin
```

Or, without root:

```bash
cp shard ~/.local/bin/
```

Check that it works:

```bash
shard version
```

## What has to be true on the other machines

Nothing has to be installed on them. They need:

- an SSH server you can log in to,
- a POSIX shell (`/bin/sh`), which every Linux and macOS install has.

That is all. The machine you type commands on is the only one that needs the
shard binary.

## SSH access without a password

shard never asks for a password: it runs SSH in batch mode, so a machine that
wants a password is treated as unreachable. Set up key-based login once per
machine:

```bash
ssh-keygen -t ed25519             # if you do not have a key yet
ssh-copy-id you@192.168.1.15      # copy it to the other machine
ssh you@192.168.1.15 "uptime"     # should print, without asking anything
```

If that last command prints the uptime without a prompt, shard can use the
machine.

> [!TIP]
> Cloud machines usually come with a key file instead. Test them the same way:
> `ssh -i ~/.ssh/aws-key.pem ubuntu@your-host "uptime"`.

## Uninstalling

```bash
sudo make uninstall
rm -rf ~/.shard          # logs and saved health checks
```

Nothing else is left behind, on your machine or on any other.
