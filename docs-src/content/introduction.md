---
title: Introduction
description: What shard is, and when it helps
order: 1
---

shard runs commands on several machines at once.

You give it a list of machines you can already reach over SSH — a laptop, an
old desktop in the corner, a cloud VM you rent by the hour — and it treats
them as one pool of computing power. Then you send work to them:

```bash
shard exec "make -j4" --on all
```

That is the whole idea. There is no cluster to set up, no scheduler to
configure, and nothing to install on the machines you use.

## When it helps

- You have machines sitting idle and a job that takes too long on one of them.
- You need to run the same thing a thousand times with a different number each
  time, and one machine would take all night.
- Your test suite runs for twenty minutes and could run on three machines
  instead of one.
- You need the same command run on every machine you own — an update, a check,
  a cleanup.
- You want to see, in one place, what each machine is doing right now — in a
  browser or in the terminal.
- You have a stack of jobs to get through and want them run in the order that
  matters, without starting each one by hand.

## What makes it small

**Nothing is installed on the far machines.** shard opens an ordinary SSH
connection and runs your command. If `ssh othermachine "uptime"` works today,
shard works today. Your own code travels with the job — see
[Shipping your code](/guide/shipping-code/) — so the other machines lend you
their CPU without needing to know anything about what they are running.

**One file describes the cluster.** `cluster.toml` lists the machines. You can
read it, edit it in any text editor, and keep it next to your project.

**One binary does everything.** The command line, the parallel execution and
the web dashboard are all the same program. No runtime, no dependencies, no
background service.

**Your machines stay yours.** Home machines and cloud machines sit in the same
list. Nothing is tied to a provider, and no account is required.

## What it is not

shard is built for batch work: jobs that start, do something, and finish. It
is a good fit for builds, test suites, batches of simulations, renders and
maintenance commands.

It is not a replacement for Kubernetes, not a real-time system, and not a
machine-learning training framework. If you need containers that restart
themselves, or millisecond scheduling, use the tools built for that.

## Where to go next

Start with [Installation](/installation/), then follow the
[Quick start](/quick-start/) — it takes about five minutes and ends with a
command running on every machine you own.
