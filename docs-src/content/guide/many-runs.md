---
title: Running one thing many times
description: shard map, for a thousand variations of the same command
order: 5
---

Some work is one command repeated with a different number each time: a
thousand simulations with a different seed, a hundred renders of different
frames, fifty test runs with different parameters.

```bash
shard map "./simulate --seed {i}" --count 1000
```

`{i}` is replaced with the run's number. The runs are spread across your
machines and happen at the same time.

## What the numbers are

By default they start at 0, so `--count 1000` gives you 0 to 999. To start
somewhere else:

```bash
shard map "./render --frame {i}" --count 240 --start 1
```

That runs frames 1 to 240. `{index}` works as well as `{i}`, if you prefer the
longer name.

## How the work is spread

Each machine runs several items at once — as many as the `cpu` you gave it in
`cluster.toml`. Each of those is called a worker, and each worker is given one
item at a time: when it reports an item finished, it gets the next one.

```
1000 items across 3 nodes, 14 workers, handed out as they finish
```

While it runs, one line keeps count:

```
  247/1000 items · about 18m left
```

And at the end, what each machine actually did:

```
  laptop           8 workers · 612 items
  old-desktop      2 workers · 96 items
  ec2-worker       4 workers · 292 items

1000/1000 items completed in 22m 41s
```

Those numbers are measured, not planned. A machine that turns out to be slow
takes fewer items; one that is free takes more; and nothing sits idle at the
end waiting for a straggler. Two items are kept in front of every worker, so
none of them waits for the network between one item and the next.

This is also why a map of a thousand items does not open a thousand SSH
connections. It opens one per worker — fourteen, above — and each worker takes
its items down that single connection.

> [!TIP]
> To use fewer machines at once, or to be gentle on a shared network,
> `--jobs 4` limits how many workers run at the same time. The rest wait
> their turn.
>
> `--static` goes back to deciding every worker's share before the run starts,
> in proportion to `cpu`. It needs no coordination and gives the same items to
> the same workers every time, at the cost of finishing no faster than its
> slowest share.

## Trying failed items again

`--retry 1` gives every failed item a second chance — and hands it to a
different machine than the one that failed it, because the machine is a common
reason for the failure:

```bash
shard map "./simulate --seed {i}" --count 1000 --retry 1
```

```
  laptop           8 workers · 612 items
  ec2-worker       4 workers · 392 items
  7 items tried again

1000/1000 items completed in 22m 41s
```

An item that fails every attempt is counted as failed; the rest of the run is
unaffected. If a machine drops out entirely — the connection dies, someone
closes the lid — the items it was holding go back into the queue and the other
machines pick them up.

## When an item fails

The other items carry on. The run is reported as failed and the count says how
many:

```
  laptop           8 workers · 612 items
  2 of 1000 items failed

998/1000 items completed in 22m 41s, 2 failed
```

Which items failed is in the logs — your command's own messages, exactly as it
printed them, in the log of the worker that ran them:

```bash
shard logs last --node laptop-3
```

## Making the numbers mean something

`{i}` is a plain number, so anything you can compute from a number works:

```bash
# every file in a numbered set
shard map "convert frames/frame{i}.png -resize 50% out/frame{i}.png" --count 500

# a parameter sweep, computed in the shell
shard map "./train --lr \$(echo '0.001 * {i}' | bc)" --count 20 --start 1
```

Quote the command so your own shell leaves `$` and `{}` alone, as above.

## Letting shard pick how many workers

By default each machine runs as many workers as the `cpu` you gave it in
`cluster.toml`. `--balance` measures the machines first and sizes each one to
the cores it has **free right now**, so a machine already busy with something
else takes fewer:

```bash
shard map "python3 sim.py --seed {i}" --count 1000 --on cpu --balance --with sim.py
```

```
measuring load…
  ● laptop: 6 of 8 cores free → 6 workers
  ● old-desktop: 2 of 2 cores free → 2 workers
  ○ ec2-worker skipped (offline)
1000 items across 2 nodes, 8 workers, handed out as they finish
```

It uses the machine's real core count, not the number you wrote, and a machine
that does not answer is dropped from the run rather than failing every item on
it. The measurement is one quick round trip before the work starts.

`--balance` and dynamic dispatch work towards the same thing from two ends: one
decides how many workers to start on each machine, the other keeps feeding
whichever workers are quickest. Together they mean you rarely have to tune the
`cpu` numbers by hand.

## When not to use it

If each item takes milliseconds, the round trip that asks for the next one
costs more than the item does. Map earns its keep when items take seconds or
minutes; below that, hand out bigger pieces of work instead.

If the items are all different commands rather than one command with a
changing number, you want [Sharing work out](/guide/sharing-work/) instead:
put them in a file, one per line, and use `--distribute`.
