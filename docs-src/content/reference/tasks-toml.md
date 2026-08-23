---
title: tasks.toml
description: Every key a named job can set
order: 3
---

`tasks.toml` holds work you run more than once. It is looked for in the
current directory, then in `~/.shard/`; `--tasks <file>` or `SHARD_TASKS`
override that.

Each job is one `[[task]]` block, optionally followed by its `[[task.step]]`
blocks. See [Named jobs](/guide/tasks-file/) for the guided version.

## [[task]]

<table>
  <tr><th>Key</th><th>Default</th><th>Meaning</th></tr>
  <tr><td><code>name</code></td><td>required</td>
      <td>What you type: <code>shard run &lt;name&gt;</code>.</td></tr>
  <tr><td><code>description</code></td><td>none</td>
      <td>One line, shown by <code>shard tasks</code>.</td></tr>
  <tr><td><code>strategy</code></td><td>implied</td>
      <td><code>broadcast</code>, <code>map</code>, <code>distribute</code>, <code>steps</code> or <code>pipeline</code>.</td></tr>
  <tr><td><code>cmd</code></td><td>none</td>
      <td>The command, for <code>broadcast</code> and <code>map</code>.</td></tr>
  <tr><td><code>script</code></td><td>none</td>
      <td>The file of commands, for <code>distribute</code>.</td></tr>
  <tr><td><code>count</code></td><td>0</td>
      <td>How many times to run, for <code>map</code>.</td></tr>
  <tr><td><code>start</code></td><td>0</td>
      <td>The first number given to <code>{i}</code>, for <code>map</code>.</td></tr>
  <tr><td><code>on</code></td><td><code>all</code></td>
      <td>Machines to use: a name, a tag, or a list of either.</td></tr>
  <tr><td><code>timeout</code></td><td>none</td>
      <td>Seconds before a job is killed.</td></tr>
  <tr><td><code>jobs</code></td><td>all</td>
      <td>How many jobs may run at once.</td></tr>
  <tr><td><code>retry</code></td><td>0</td>
      <td>Extra attempts for a failed job or item.</td></tr>
  <tr><td><code>with</code></td><td>none</td>
      <td>Files or directories to ship to each remote worker before it runs.</td></tr>
</table>

An absent `strategy` is worked out from what the task holds: steps mean
`steps`, a `count` means `map`, a `script` means `distribute`, and a bare
`cmd` means `broadcast`.

## [[task.step]]

<table>
  <tr><th>Key</th><th>Default</th><th>Meaning</th></tr>
  <tr><td><code>name</code></td><td>its number</td>
      <td>Shown in the output and used for the log file name.</td></tr>
  <tr><td><code>cmd</code></td><td>required</td><td>What this step runs.</td></tr>
  <tr><td><code>on</code></td><td>the task's <code>on</code></td>
      <td>Machines this step may use. It runs on one of them.</td></tr>
  <tr><td><code>produces</code></td><td>none</td>
      <td>Files the step leaves behind. In a pipeline they are carried to the machines the later steps run on.</td></tr>
</table>

Steps belong to the `[[task]]` above them, which is what the double brackets
already mean in TOML.

## A file using all of it

```toml
[[task]]
name        = "update-all"
description = "Bring every machine up to date"
cmd         = "sudo apt-get update && sudo apt-get -y upgrade"
on          = ["cpu", "cloud"]
timeout     = 600

[[task]]
name        = "simulations"
strategy    = "map"
count       = 1000
start       = 1
cmd         = "./simulate --seed {i}"
on          = ["cpu"]

[[task]]
name        = "test-suite"
strategy    = "distribute"
script      = "tests.sh"

[[task]]
name        = "release"
strategy    = "pipeline"

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

## Overriding a task

Options typed on the command line win over the file, for that run only:

```bash
shard run simulations --on cloud --dry-run
```
