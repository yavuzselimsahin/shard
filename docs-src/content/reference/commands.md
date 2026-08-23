---
title: Commands
description: Every command and option
order: 1
---

```
shard <command> [arguments] [options]
```

`shard help` prints a short version of this page. `shard version` prints the
version.

## Cluster

<table>
  <tr><th>Command</th><th>What it does</th></tr>
  <tr><td><code>shard cluster init</code></td>
      <td>Writes a <code>cluster.toml</code> in the current directory, containing this machine.</td></tr>
  <tr><td><code>shard cluster add-node</code></td>
      <td>Asks for a machine's details and appends it to the file.</td></tr>
  <tr><td><code>shard cluster list</code></td>
      <td>Shows the machines, their addresses, cores and tags.</td></tr>
  <tr><td><code>shard cluster health</code></td>
      <td>Contacts every machine and reports which answered, with load and memory.</td></tr>
  <tr><td><code>shard cluster remove &lt;name&gt;</code></td>
      <td>Removes one machine's block from the file, leaving your comments intact.</td></tr>
</table>

`shard cluster health --on <where>` checks part of the cluster.

## Running work

```bash
shard exec "<command>" [--on <where>] [options]
shard exec-script <file> [--distribute] [--on <where>] [options]
shard map "<command with {i}>" --count <n> [--start <n>] [options]
shard pipeline --step "<command>" [--on <where>] [--name <label>] [--produces <path>] … [--parallel]
shard run <task-name> [options]
```

`exec` runs one command on each selected machine. `exec-script` sends a file
to each machine's shell; with `--distribute` it shares the file's lines out
between them instead. `map` runs one command many times with a changing
number, spread across the machines. `pipeline` runs different commands in
order, each on a machine of its own — `--parallel` starts them all at once
instead. `run` does whichever of these a task in
[tasks.toml](/reference/tasks-toml/) asks for.

<table>
  <tr><th>Option</th><th>Meaning</th></tr>
  <tr><td><code>--on &lt;where&gt;</code></td>
      <td>Machines to use: <code>all</code>, a name, a tag, or a comma separated list. Default <code>all</code>.</td></tr>
  <tr><td><code>--jobs &lt;n&gt;</code></td>
      <td>How many machines may work at once. Default: all of them.</td></tr>
  <tr><td><code>--timeout &lt;seconds&gt;</code></td>
      <td>Kill a machine's job after this long and report it as timed out.</td></tr>
  <tr><td><code>--stream</code></td>
      <td>Print output as it arrives, each line prefixed with its machine.</td></tr>
  <tr><td><code>--dry-run</code></td>
      <td>Print the command line for each machine and run nothing.</td></tr>
  <tr><td><code>--no-log</code></td>
      <td>Do not write a log directory for this run.</td></tr>
  <tr><td><code>--distribute</code></td>
      <td><code>exec-script</code> only: split the file's lines between machines.</td></tr>
  <tr><td><code>--count &lt;n&gt;</code></td>
      <td><code>map</code> only: how many times to run the command. Required.</td></tr>
  <tr><td><code>--start &lt;n&gt;</code></td>
      <td><code>map</code> only: the first number given to <code>{i}</code>. Default 0.</td></tr>
  <tr><td><code>--static</code></td>
      <td><code>map</code> and <code>--distribute</code>: decide every machine's share before the run starts, instead of handing work out as machines finish.</td></tr>
  <tr><td><code>--retry &lt;n&gt;</code></td>
      <td>Extra attempts for a failed job, or for a failed item in a <code>map</code> or distributed run.</td></tr>
  <tr><td><code>--with &lt;file&gt;</code></td>
      <td>Ship this file or directory to each remote worker before it runs, and remove it after. May be repeated.</td></tr>
  <tr><td><code>--keep</code></td>
      <td>Leave the shipped copy on the workers instead of cleaning it up.</td></tr>
  <tr><td><code>--balance</code></td>
      <td>Measure each machine's live load first; size the work to the cores it has free, and drop machines that do not answer. For <code>map</code> and <code>--distribute</code>.</td></tr>
  <tr><td><code>--step &lt;command&gt;</code></td>
      <td><code>pipeline</code> only: one step. An <code>--on</code>, <code>--name</code> or <code>--produces</code> after it belongs to that step.</td></tr>
  <tr><td><code>--produces &lt;path&gt;</code></td>
      <td><code>pipeline</code> only: a file the step leaves behind, carried to the machines the later steps run on. May be repeated.</td></tr>
  <tr><td><code>--parallel</code></td>
      <td><code>pipeline</code> only: start every step at once instead of in order.</td></tr>
  <tr><td><code>--tasks &lt;file&gt;</code></td>
      <td><code>run</code> and <code>tasks</code>: use this file instead of searching for <code>tasks.toml</code>.</td></tr>
</table>

## Named jobs

<table>
  <tr><th>Command</th><th>What it does</th></tr>
  <tr><td><code>shard tasks</code></td>
      <td>Lists the jobs in <code>tasks.toml</code>, with their strategy and steps.</td></tr>
  <tr><td><code>shard run &lt;name&gt;</code></td>
      <td>Runs one of them. Options you type override what the file says.</td></tr>
</table>

`shard run` with no name lists the tasks, in case you forgot one.

## The queue

```bash
shard queue add [--priority <n>] [--name "<label>"] -- <command>
shard queue list
shard queue run [--stop-on-fail]
shard queue remove <id>
shard queue clear
```

<table>
  <tr><th>Command</th><th>What it does</th></tr>
  <tr><td><code>queue add -- &lt;command&gt;</code></td>
      <td>Puts a job on the queue. Everything after <code>--</code> is the command to run, exactly as you would type it after <code>shard</code>.</td></tr>
  <tr><td><code>queue list</code></td>
      <td>Shows the waiting jobs, in the order they will run.</td></tr>
  <tr><td><code>queue run</code></td>
      <td>Runs the jobs one at a time, highest priority first, until the queue is empty.</td></tr>
  <tr><td><code>queue remove &lt;id&gt;</code></td>
      <td>Drops one waiting job. The id is shown by <code>queue list</code>.</td></tr>
  <tr><td><code>queue clear</code></td>
      <td>Empties the queue.</td></tr>
</table>

<table>
  <tr><th>Option</th><th>Meaning</th></tr>
  <tr><td><code>--priority &lt;n&gt;</code></td>
      <td><code>queue add</code>: higher runs sooner. Default 0. Ties break first-in, first-out.</td></tr>
  <tr><td><code>--name &lt;label&gt;</code></td>
      <td><code>queue add</code>: a label for the listing and the run.</td></tr>
  <tr><td><code>--stop-on-fail</code></td>
      <td><code>queue run</code>: stop the moment a job fails, leaving the rest queued.</td></tr>
</table>

Jobs live under `~/.shard/queue`, one file each, so the queue survives the
program exiting.

## Looking at runs

<table>
  <tr><th>Command</th><th>What it does</th></tr>
  <tr><td><code>shard status</code></td>
      <td>Runs that are going on right now.</td></tr>
  <tr><td><code>shard logs [&lt;id&gt;]</code></td>
      <td>Output of a run. With no id, or <code>last</code>, the most recent one.</td></tr>
  <tr><td><code>shard logs &lt;id&gt; --node &lt;name&gt;</code></td>
      <td>Output from one machine only.</td></tr>
  <tr><td><code>shard history [-n &lt;count&gt;]</code></td>
      <td>Past runs, newest first. Default 20.</td></tr>
</table>

## The dashboards

```bash
shard ui [port]
shard tui
```

`ui` serves the dashboard on `localhost`, port 8787 unless you say otherwise —
see [The web dashboard](/guide/web-dashboard/). `tui` draws the same view in
the terminal, and needs one: with its output redirected it says so and stops.
See [The terminal dashboard](/guide/terminal-dashboard/).

## Global options

<table>
  <tr><th>Option</th><th>Meaning</th></tr>
  <tr><td><code>--config &lt;file&gt;</code></td>
      <td>Use this cluster file instead of searching for one.</td></tr>
  <tr><td><code>--help</code>, <code>help</code></td><td>Usage summary.</td></tr>
  <tr><td><code>version</code></td><td>Version number.</td></tr>
</table>

The `SHARD_CONFIG` environment variable does the same as `--config`, for a
whole shell session, and `SHARD_TASKS` does the same for `--tasks`:

```bash
export SHARD_CONFIG=~/work/build-farm.toml
export SHARD_TASKS=~/work/jobs.toml
```
