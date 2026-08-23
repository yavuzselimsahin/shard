---
title: What is not here yet
description: The honest list
order: 8
---

This is version 0.1. It does one thing properly — run commands on machines you
reach over SSH — and the rest is still ahead. Knowing the difference saves you
looking for something that is not there.

## Works today

- A cluster described in one file, mixing home and cloud machines freely.
- `shard exec`: the same command on every selected machine, in parallel.
- `shard exec-script --distribute`: one list of jobs, split by machine capacity.
- `shard map`: one command run many times with a changing number, spread over
  every core you own.
- `shard pipeline`: different commands in order, each on a machine of its own,
  stopping at the first failure — or side by side with `--parallel`.
- `tasks.toml` and `shard run <name>`: the work you repeat, written down.
- Work handed out as machines finish, so a slow machine takes less rather than
  holding up the run.
- `--retry`, which gives a failed job or item another go — a failed item on a
  different machine than the one that failed it.
- `produces`, which carries a step's files to the machine the next step runs
  on.
- Two dashboards over the same log directory: one in a browser, one in the
  terminal.
- `--with`, which ships your own code to the machines so they can run a job
  they have never seen.
- `--balance`, which measures live load and places work on the machines that
  are actually free.
- A priority queue, so several jobs can be lined up and run in the order that
  matters to you.
- Health checks with load and memory.
- Logs, history and per-machine output on disk.
- The read-only web dashboard.

## Not yet

**An agent.** Everything today is a fresh SSH connection per job. A small
program running on each machine would make queuing, health monitoring and
reconnection cheaper, at the cost of having something to install.

**Placement by real load, always on.** `--balance` measures load when you ask
for it; without it, placement still uses the `cpu` number you wrote. Making the
measurement continuous — so a long run notices a machine getting busy partway
through — is not done.

**Files between machines directly.** What a step produces travels through this
machine in two hops. Two cloud machines that can reach each other could pass
it across in one.

**Cloud provisioning.** shard uses machines; it does not create them. Creating
and destroying cloud machines from the command line is planned, deliberately
after everything above.

## Things it will not do

It will not become a container orchestrator, a real-time scheduler, or a
distributed filesystem. Those exist, they are good, and they are large. The
point of shard is to stay small enough to read.
