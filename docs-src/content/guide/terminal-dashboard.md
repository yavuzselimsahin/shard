---
title: The terminal dashboard
description: The same view, without leaving the terminal
order: 11
---

```bash
shard tui
```

Everything the [web dashboard](/guide/web-dashboard/) shows, drawn in the
terminal you are already in. It is the right one to use over SSH, on a machine
with no browser, or when you would rather not leave the keyboard.

```
 shard  home-lab  3/4 online                                        21:47:03
────────────────────────────────────────────────────────────────────────────
 NODES
 ● laptop           ▓▓░░░░░░  22% cpu  ▓▓▓▓░░░░  46% ram  8 cpu
 ● old-desktop      ░░░░░░░░   4% cpu  ▓▓░░░░░░  21% ram  2 cpu
 ● ec2-worker       ▓▓▓▓▓▓░░  71% cpu  ▓▓▓▓░░░░  50% ram  4 cpu
 ○ gcp-worker       offline

 RUNS
 running     ./simulate --seed {i}              247/1000 items   18m 12s  21:28
 completed   make -j4                                3/3 jobs    5m 03s  20:55
 failed      ./deploy.sh                             1/2 jobs      10.0s  20:41
 interrupted pytest tests/                            4/9 items   1m 20s  19:02

 [q]uit  [j/k] move  [enter] open  [h]ealth check  [r]efresh
```

## Reading it

**Nodes** come from `cluster.toml`, and the bars from the last health check.
Press **h** to run one now — it is the same check `shard cluster health` does,
and the web dashboard shows the result too.

**Runs** are every run in the log directory, newest first. A run started in
another terminal appears here within a second.

A run marked `interrupted` said it was running, but the process that started
it is gone — a closed terminal, a laptop that slept. shard asks the operating
system rather than believing the file.

## Looking inside a run

Move with **j** and **k** (or the arrow keys) and press **Enter**:

```
 20260822-212811-4021  running  ./simulate --seed {i}   247/1000 items · 18m 12s
 ▓▓▓▓▓▓░░░░░░░░░░░░░░░░░░ 24%

 laptop#1                 running      18m 10s
 laptop#2                 running      18m 10s
 old-desktop#1            running      18m 09s
 ec2-worker#1             completed     2m 41s

 seed 612 done in 4.1s
 seed 613 done in 3.9s
 …

 [esc] back  [n/p] job  [j/k] scroll log  [g/G] top/end  [q]uit
```

The log at the bottom belongs to the highlighted machine and keeps up with the
run as it goes. **n** and **p** move between machines, **j** and **k** scroll
back through the output, **G** returns to the end, and **Esc** goes back to
the list.

## Keys

<table>
  <tr><th>Key</th><th>In the list</th><th>Inside a run</th></tr>
  <tr><td><code>j</code> / <code>k</code>, arrows</td><td>move between runs</td><td>scroll the log</td></tr>
  <tr><td><code>Enter</code></td><td>open the run</td><td>—</td></tr>
  <tr><td><code>Esc</code></td><td>—</td><td>back to the list</td></tr>
  <tr><td><code>n</code> / <code>p</code></td><td>—</td><td>next / previous machine</td></tr>
  <tr><td><code>g</code> / <code>G</code></td><td>—</td><td>start / end of the log</td></tr>
  <tr><td><code>h</code></td><td>run a health check</td><td>—</td></tr>
  <tr><td><code>r</code></td><td>reload now</td><td>—</td></tr>
  <tr><td><code>q</code></td><td>quit</td><td>quit</td></tr>
</table>

## It only reads

Like the web dashboard, it cannot start work — it shows what is happening and
what has happened. The one thing it changes is the health check, which is a
read too.

## If it will not start

```
shard: shard tui needs a terminal. For a browser, run: shard ui
```

That is what you get when the output is a pipe or a file rather than a
terminal — in a cron job or a CI log, for instance. `shard status` and
`shard history` are the plain-text versions.
