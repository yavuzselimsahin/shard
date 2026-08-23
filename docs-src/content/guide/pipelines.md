---
title: Steps and pipelines
description: Different commands, in order or side by side
order: 6
---

`exec` runs one command; `map` runs one command many times. Sometimes the work
is several *different* commands, and what matters is where each one runs and
in what order.

## Steps in order

```bash
shard pipeline \
  --step "./download.sh"  --on cloud \
  --step "./process.sh"   --on fast \
  --step "./upload.sh"    --on cloud
```

Each step runs on a machine matching its `--on`, and the next step starts only
after the previous one succeeded:

```
pipeline: 3 steps, in order
[ec2-worker·1]  ./download.sh    ✓ completed (41.20s)
[laptop·2]      ./process.sh     ✓ completed (3m 12s)
[ec2-worker·3]  ./upload.sh      ✓ completed (52.90s)

3/3 completed in 4m 46s
```

## When a step fails

The pipeline stops. The steps after it are marked as not run rather than
started, because they were written expecting the earlier one to have worked:

```
[laptop·1]      ./build.sh       ✓ completed (2m 01s)
[laptop·2]      ./test.sh        ✗ failed (exit 1) (48.10s)
[ec2-worker·3]  ./deploy.sh      – skipped (0.00s)

1/3 succeeded in 2m 49s, 1 failed, 1 not run
```

`shard pipeline` exits with status 1, so a script wrapping it stops too.

## Carrying files to the next step

A step can name the files it leaves behind. They are collected when the step
succeeds and put on the machine the next steps run on, before they start:

```bash
shard pipeline \
  --step "make release" --on fast  --produces "dist/app.tar.gz" \
  --step "./deploy.sh dist/app.tar.gz" --on cloud
```

```
pipeline: 2 steps, in order
[laptop·1]      make release      ✓ completed (2m 01s)
  ↑ dist/app.tar.gz from laptop (14.2 MB)
  ↓ dist/app.tar.gz onto ec2-worker
[ec2-worker·2]  ./deploy.sh …     ✓ completed (38.40s)
```

Repeat `--produces` for more than one file. Directories work too — the whole
tree travels.

**Where files land.** Paths are relative to where the step ran: the machine's
`workdir` if it has one, the login directory otherwise. A file collected from
`dist/app.tar.gz` on one machine is unpacked as `dist/app.tar.gz` on the next.

**How they travel.** Through this machine, in two hops: shard packs them into
a tar, pulls that back over the same SSH connection it already uses, and
unpacks it on the far side. Nothing has to be installed, and the two machines
never need to reach each other — which is the normal case for a laptop at home
and a VM in a data centre.

> [!NOTE]
> A step that names a file it did not produce stops the pipeline, rather than
> letting the next step run without it. The steps after it are marked as not
> run, exactly as if the step itself had failed.

Big artifacts cross the network twice, so for hundreds of megabytes between
two cloud machines an `scp` written into the step itself will be faster. For
build output and reports, which is what most pipelines carry, two hops is
fine.

## Steps side by side

With `--parallel`, the steps start at the same time instead:

```bash
shard pipeline --parallel \
  --step "make TARGET=linux"   --on cpu \
  --step "make TARGET=macos"   --on laptop \
  --step "make TARGET=windows" --on cpu
```

Use it when the steps are independent — three builds of the same project, say.
Nothing waits for anything, and one failure does not stop the others. Files
are not carried between steps that run at the same time: there is no "next
step" to carry them to.

## Which machine a step lands on

`--on` names a group; the step runs on one machine from it. shard picks the
machine in that group that has taken the fewest steps so far, so three steps
tagged `cpu` spread over your cpu machines instead of piling onto the first
one.

To pin a step to a specific machine, name it: `--on laptop`.

## Naming steps

Steps are numbered in the output. `--name` gives one a word instead, which
makes the log easier to read afterwards:

```bash
shard pipeline \
  --step "./download.sh" --name fetch   --on cloud \
  --step "./process.sh"  --name process --on fast
```

```
[ec2-worker·fetch]  …
[laptop·process]    …
```

## Writing it down

A pipeline you run often does not want retyping. Put it in `tasks.toml` and it
becomes `shard run release` — see [Named jobs](/guide/tasks-file/).
