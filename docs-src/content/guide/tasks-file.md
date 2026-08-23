---
title: Named jobs
description: tasks.toml, so you type the name instead of the command
order: 7
---

Work you do more than once belongs in a file. `tasks.toml` sits next to
`cluster.toml` and holds named jobs:

```bash
shard tasks              # what is written down
shard run build-all      # run one of them
```

## The file

```toml
[[task]]
name        = "update-all"
description = "Bring every machine up to date"
cmd         = "sudo apt-get update && sudo apt-get -y upgrade"
on          = ["cpu"]

[[task]]
name        = "test-suite"
description = "The whole suite, split across the cluster"
strategy    = "distribute"
script      = "tests.sh"

[[task]]
name        = "simulations"
description = "1000 runs with different seeds"
strategy    = "map"
count       = 1000
cmd         = "./simulate --seed {i}"
on          = ["cpu"]

[[task]]
name        = "release"
description = "Build, test, publish — in that order"
strategy    = "pipeline"

  [[task.step]]
  name = "build"
  cmd  = "make release"
  on   = ["fast"]

  [[task.step]]
  name = "test"
  cmd  = "make test"
  on   = ["fast"]

  [[task.step]]
  name = "publish"
  cmd  = "./publish.sh"
  on   = ["cloud"]
```

`shard tasks` prints exactly this, in short:

```
tasks.toml — 4 tasks

update-all        broadcast   Bring every machine up to date
  sudo apt-get update && sudo apt-get -y upgrade
test-suite        distribute  The whole suite, split across the cluster
  tests.sh
simulations       map         1000 runs with different seeds
  ./simulate --seed {i}
release           pipeline    Build, test, publish — in that order
  build           make release
  test            make test
  publish         ./publish.sh
```

## The five strategies

<table>
  <tr><th>strategy</th><th>What the task needs</th><th>What happens</th></tr>
  <tr><td><code>broadcast</code></td><td><code>cmd</code></td>
      <td>The command runs on every selected machine.</td></tr>
  <tr><td><code>map</code></td><td><code>cmd</code> with <code>{i}</code>, <code>count</code></td>
      <td>The command runs <code>count</code> times, spread over the machines.</td></tr>
  <tr><td><code>distribute</code></td><td><code>script</code></td>
      <td>The file's lines are shared out between the machines.</td></tr>
  <tr><td><code>steps</code></td><td><code>[[task.step]]</code> blocks</td>
      <td>Every step starts at the same time, each on its own machine.</td></tr>
  <tr><td><code>pipeline</code></td><td><code>[[task.step]]</code> blocks</td>
      <td>The steps run in order and stop at the first failure.</td></tr>
</table>

You can leave `strategy` out: a task with steps is `steps`, one with a `count`
is `map`, one with a `script` is `distribute`, and one with just a `cmd` is
`broadcast`.

## What a task may set

<table>
  <tr><th>Key</th><th>Meaning</th></tr>
  <tr><td><code>name</code></td><td>What you type after <code>shard run</code>.</td></tr>
  <tr><td><code>description</code></td><td>One line, shown by <code>shard tasks</code>.</td></tr>
  <tr><td><code>strategy</code></td><td>One of the five above.</td></tr>
  <tr><td><code>cmd</code></td><td>The command, for <code>broadcast</code> and <code>map</code>.</td></tr>
  <tr><td><code>script</code></td><td>The file, for <code>distribute</code>.</td></tr>
  <tr><td><code>count</code>, <code>start</code></td><td>How many items, and the first number, for <code>map</code>.</td></tr>
  <tr><td><code>on</code></td><td>Which machines: a name, a tag, or a list of them.</td></tr>
  <tr><td><code>timeout</code>, <code>jobs</code></td><td>The same limits the command line takes.</td></tr>
  <tr><td><code>retry</code></td><td>Extra attempts for a failed job or item. Default none.</td></tr>
</table>

A `[[task.step]]` takes `name`, `cmd`, its own `on`, and `produces` — the
files it leaves behind, which are carried to the machine the next step runs
on:

```toml
  [[task.step]]
  name     = "build"
  cmd      = "make release"
  on       = ["fast"]
  produces = ["dist/app.tar.gz"]

  [[task.step]]
  name = "publish"
  cmd  = "./publish.sh dist/app.tar.gz"
  on   = ["cloud"]
```

See [Carrying files to the next step](/guide/pipelines/) for how that
travels.

## Changing your mind at the keyboard

Anything you type wins over what the file says:

```bash
shard run simulations --on cloud     # somewhere else, just this once
shard run release --dry-run          # show the steps, run nothing
shard run test-suite --stream        # watch it happen
```

## Where the file is looked for

`tasks.toml` in the current directory, then `~/.shard/tasks.toml`. Pass
`--tasks <file>` to use another one, or set `SHARD_TASKS`.

Keeping it next to the project it belongs to is usually right: the tasks
travel with the code, and someone who clones the repository can read what you
run without asking.
