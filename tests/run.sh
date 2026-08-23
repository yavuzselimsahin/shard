#!/bin/sh
# A smoke test that needs no remote machines: every node in the test cluster
# is localhost, which shard runs without SSH.
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
bin="$root/shard"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

export HOME="$work/home"
mkdir -p "$HOME"
cd "$work"

pass=0
fail=0

check() {
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1))
        printf '  ok   %s\n' "$1"
    else
        fail=$((fail + 1))
        printf '  FAIL %s\n       expected: %s\n       got:      %s\n' "$1" "$3" "$2"
    fi
}

contains() {
    case "$2" in
        *"$3"*) pass=$((pass + 1)); printf '  ok   %s\n' "$1" ;;
        *) fail=$((fail + 1)); printf '  FAIL %s\n       %s not in output\n' "$1" "$3" ;;
    esac
}

echo "cluster file"
"$bin" cluster init > /dev/null
check "init creates cluster.toml" "$([ -f cluster.toml ] && echo yes)" "yes"

cat > cluster.toml <<'EOF'
[cluster]
name = "test-lab"

[[nodes]]
name = "a"
host = "localhost"
tags = ["cpu"]
cpu  = 3

[[nodes]]
name = "b"
host = "localhost"
tags = ["cpu", "spare"]
cpu  = 1
EOF

contains "list shows both nodes" "$("$bin" cluster list)" "b"
contains "health reaches localhost" "$("$bin" cluster health)" "2/2 online"

echo "exec"
contains "runs on every node" "$("$bin" exec 'echo hello' --on all)" "2/2 completed"
contains "tag selects nodes" "$("$bin" exec 'echo hi' --on spare)" "1/1 completed"
contains "streams output" "$("$bin" exec 'echo streamed' --on a --stream)" "[a] streamed"
"$bin" exec 'exit 4' --on a > /dev/null 2>&1 && rc=0 || rc=$?
check "a failing command fails the run" "$rc" "1"

echo "exec-script"
seq 1 8 | sed 's/^/echo item /' > work.sh
out=$("$bin" exec-script work.sh --distribute --on cpu)
contains "hands lines out as machines finish" "$out" "8/8 items completed"
contains "every line ran" "$("$bin" logs last)" "item 8"
contains "the dispatch chatter stays out of the log" \
    "$("$bin" logs last | grep -c __shard_item__ || true)" "0"
out=$("$bin" exec-script work.sh --distribute --on cpu --static)
contains "--static splits by cpu weight" "$out" "lines 1-6"
contains "--static gives the rest to the smaller node" "$out" "lines 7-8"
contains "broadcast sends the whole script" \
    "$("$bin" exec-script work.sh --on a)" "1/1 completed"

echo "map"
out=$("$bin" map 'echo item {i}' --count 6 --on all)
contains "spreads the items over the workers" "$out" "6 items across 2 nodes"
contains "every item ran" "$("$bin" logs last)" "item 5"
contains "the first item is index 0" "$("$bin" logs last)" "item 0"
contains "--start moves the first index" \
    "$("$bin" map 'echo n{i}' --count 2 --start 100 --on a; "$bin" logs last)" "n101"
"$bin" map 'test {i} -lt 2' --count 4 --on a > /dev/null 2>&1 && rc=0 || rc=$?
check "a failing item fails the run" "$rc" "1"
contains "counts the failures" "$("$bin" map 'test {i} -lt 2' --count 4 --on a)" "2 failed"
contains "--count is required" "$("$bin" map 'echo {i}' 2>&1)" "--count"
contains "items are counted, not workers" \
    "$("$bin" map 'echo n{i}' --count 5 --on all)" "5/5 items completed"
contains "--static still shares the work out up front" \
    "$("$bin" map 'echo n{i}' --count 4 --on all --static)" "4 items across 2 nodes"
contains "a dry run shows the items" \
    "$("$bin" map 'echo n{i}' --count 4 --on a --dry-run)" "echo n3"

echo "retry"
# Fails the first time it sees an item, works the second: exactly what a
# retry is supposed to rescue.
rm -f flag-*
out=$("$bin" map 'if [ -f flag-{i} ]; then exit 0; else touch flag-{i}; exit 1; fi' \
      --count 4 --on all --retry 1)
contains "a retried item ends up completing" "$out" "4/4 items completed"
contains "the retries are counted" "$out" "4 items tried again"
rm -f flag-*

out=$("$bin" map 'exit 1' --count 2 --on a --retry 2 2>&1) || true
contains "retries eventually give up" "$out" "2 failed"

rm -f once
"$bin" exec 'if [ -f once ]; then exit 0; else touch once; exit 1; fi' \
    --on a --retry 1 > /dev/null 2>&1 && rc=0 || rc=$?
check "a whole job can be retried too" "$rc" "0"
rm -f once

echo "tasks.toml"
cat > tasks.toml <<'EOF'
[[task]]
name        = "greet"
description = "hello everywhere"
cmd         = "echo hello"

[[task]]
name     = "build"
strategy = "steps"

  [[task.step]]
  name = "one"
  cmd  = "echo building one"
  on   = ["cpu"]

  [[task.step]]
  name = "two"
  cmd  = "echo building two"
  on   = ["cpu"]

[[task]]
name     = "release"
strategy = "pipeline"

  [[task.step]]
  name = "first"
  cmd  = "echo first"

  [[task.step]]
  name = "second"
  cmd  = "false"

  [[task.step]]
  name = "third"
  cmd  = "echo third"

[[task]]
name     = "sims"
strategy = "map"
count    = 4
cmd      = "echo sim {index}"
EOF

contains "tasks are listed" "$("$bin" tasks)" "greet"
contains "the strategy is shown" "$("$bin" tasks)" "pipeline"
contains "a task runs everywhere" "$("$bin" run greet)" "2/2 completed"
contains "a map task uses its count" "$("$bin" run sims)" "4 items"
contains "steps go to different machines" "$("$bin" run build)" "b·two"
contains "--on overrides the task" "$("$bin" run greet --on a)" "1/1 completed"

out=$("$bin" run release 2>&1) && rc=0 || rc=$?
check "a broken pipeline fails" "$rc" "1"
contains "the pipeline stops at the failure" "$out" "not run"
contains "the later step is marked skipped" "$out" "skipped"
contains "an unknown task is refused" "$("$bin" run nosuch 2>&1)" "no task called"

echo "pipeline"
out=$("$bin" pipeline --step "echo one" --on a --step "echo two" --on b)
contains "steps run in the order given" "$out" "2/2 completed"
contains "each step names its machine" "$out" "b·"
out=$("$bin" pipeline --step "false" --name broken --step "echo after" 2>&1) || true
contains "a failed step stops the rest" "$out" "not run"

echo "shipping code to workers"
# A fake ssh proves the remote path without a real sshd: it runs the command
# under a separate HOME, so a worker only has a file if shard shipped it.
fakebin="$work/fakebin"
mkdir -p "$fakebin" "$work/rhome"
cat > "$fakebin/ssh" <<'SSH'
#!/bin/sh
eval "cmd=\${$#}"
cd "$FAKE_REMOTE_HOME" 2>/dev/null || true
HOME="$FAKE_REMOTE_HOME" exec /bin/sh -c "$cmd"
SSH
chmod +x "$fakebin/ssh"
export FAKE_REMOTE_HOME="$work/rhome"

cat > ship.toml <<EOF
[cluster]
name = "ship"
[[nodes]]
name = "w1"
host = "remotehost"
cpu  = 2
EOF
echo 'print("shipped code ran")' > payload.py

( export PATH="$fakebin:$PATH"
  "$bin" --config ship.toml exec "python3 payload.py" --on all > without.txt 2>&1 ) || true
contains "a job fails when its file was never shipped" "$(cat without.txt)" "failed"

( export PATH="$fakebin:$PATH"
  "$bin" --config ship.toml exec "python3 payload.py" --on all --with payload.py > with.txt 2>&1 ) || true
contains "shipping announces the transfer" "$(cat with.txt)" "shipped 1 file"
contains "the shipped job then succeeds" "$(cat with.txt)" "1/1 completed"
contains "the shipped output is right" "$("$bin" --config ship.toml logs last)" "shipped code ran"
check "the copy is cleaned up afterwards" \
    "$(find "$work/rhome" -name payload.py 2>/dev/null | wc -l | tr -d ' ')" "0"

( export PATH="$fakebin:$PATH"
  "$bin" --config ship.toml exec "true" --on all --with payload.py --keep > /dev/null 2>&1 ) || true
check "--keep leaves the copy in place" \
    "$(find "$work/rhome" -name payload.py 2>/dev/null | wc -l | tr -d ' ')" "1"
rm -rf "$work/rhome"

echo "load-aware placement"
contains "balance measures the machine" \
    "$("$bin" map 'echo n{i}' --count 4 --on a --balance)" "cores free"
contains "and still runs everything" \
    "$("$bin" map 'echo n{i}' --count 4 --on a --balance)" "4/4 items completed"

# With the fake ssh: a dead host is dropped, a good one is kept.
cat > bal.toml <<EOF
[cluster]
name = "bal"
[[nodes]]
name = "good"
host = "goodhost"
[[nodes]]
name = "dead"
host = "deadhost"
EOF
cat > "$fakebin/ssh" <<'SSH'
#!/bin/sh
eval "cmd=\${$#}"
for a in "$@"; do case "$a" in deadhost) echo "refused" >&2; exit 255;; esac; done
cd "$FAKE_REMOTE_HOME" 2>/dev/null || true
HOME="$FAKE_REMOTE_HOME" exec /bin/sh -c "$cmd"
SSH
chmod +x "$fakebin/ssh"
mkdir -p "$work/bh"; export FAKE_REMOTE_HOME="$work/bh"
echo 'print("ok")' > pay.py
out=$(PATH="$fakebin:$PATH" "$bin" --config bal.toml map "python3 pay.py" --count 2 --on all --balance --with pay.py 2>&1) || true
contains "an unreachable machine is skipped" "$out" "dead skipped"
contains "the run goes on with the machine that answered" "$out" "2/2 items completed"
rm -rf "$work/bh"

echo "carrying files between steps"
mkdir -p x y
cat > art.toml <<EOF
[cluster]
name = "art"

[[nodes]]
name    = "x"
host    = "localhost"
workdir = "$work/x"

[[nodes]]
name    = "y"
host    = "localhost"
workdir = "$work/y"
EOF

out=$("$bin" --config art.toml pipeline \
      --step "echo carried > out.txt" --name make --on x --produces out.txt \
      --step "cat out.txt" --name use --on y)
contains "the pipeline completes" "$out" "2/2 completed"
contains "the file is collected" "$out" "out.txt from x"
check "and it really arrives" "$(cat y/out.txt 2>/dev/null)" "carried"

out=$("$bin" --config art.toml pipeline \
      --step "true" --name make --on x --produces missing.txt \
      --step "echo second" --on y 2>&1) || true
contains "a file that is not there stops the pipeline" "$out" "not run"

echo "priority queue"
"$bin" queue clear > /dev/null 2>&1
"$bin" queue add --name low  -- exec "echo QLOW"  --on a > /dev/null
"$bin" queue add --priority 10 --name high -- exec "echo QHIGH" --on a > /dev/null
"$bin" queue add --priority 5  --name mid  -- exec "echo QMID"  --on a > /dev/null
contains "the queue lists what is waiting" "$("$bin" queue list)" "3 jobs waiting"
contains "the highest priority sorts first" \
    "$("$bin" queue list | sed -n '4p')" "high"

out=$("$bin" queue run)
# The header line of each job names it; their order is the run order.
order=$(printf '%s\n' "$out" | grep -o 'priority [0-9]*' | tr '\n' ' ')
check "jobs run highest priority first" "$order" "priority 10 priority 5 priority 0 "
contains "the run reports how many it did" "$out" "3 jobs run"
contains "the queue is empty afterwards" "$("$bin" queue list)" "empty"

"$bin" queue add --priority 9 -- exec "exit 1" --on a > /dev/null
"$bin" queue add --name later -- exec "echo LATER" --on a > /dev/null
out=$("$bin" queue run --stop-on-fail 2>&1) || true
contains "a failure with --stop-on-fail halts the queue" "$out" "still queued"
contains "and does not run the later job" "$("$bin" queue list)" "later"
"$bin" queue clear > /dev/null

echo "terminal dashboard"
contains "tui says so when there is no terminal" \
    "$("$bin" tui < /dev/null 2>&1)" "needs a terminal"
"$bin" tui < /dev/null > /dev/null 2>&1 && rc=0 || rc=$?
check "and exits with an error" "$rc" "1"

echo "live logs"
( "$bin" exec 'echo first-line; sleep 1; echo second-line' --on a > /dev/null 2>&1 & )
sleep 0.5
contains "output is on disk while the job runs" "$("$bin" logs last)" "first-line"
sleep 1.2

echo "logs"
"$bin" exec 'echo logmarker' --on a > /dev/null
contains "history lists the runs" "$("$bin" history)" "completed"
contains "logs show job output" "$("$bin" logs last)" "logmarker"
contains "status is quiet when idle" "$("$bin" status)" "Nothing is running"

echo "selection"
contains "an unknown selector is refused" \
    "$("$bin" exec 'echo x' --on nowhere 2>&1)" "no nodes matched"

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
