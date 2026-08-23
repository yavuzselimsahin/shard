#!/bin/sh
# Writes a work list with deliberately uneven line costs, so you can watch
# shard's dynamic --distribute keep every machine busy instead of one holding
# up the rest. Feed the result to:  shard exec-script worklist.sh --distribute
#
#   sh make_worklist.sh > worklist.sh
py=${PY:-python3}
here=$(cd "$(dirname "$0")" && pwd)

# 24 batches; every sixth one is ten times heavier than the others.
i=0
while [ "$i" -lt 24 ]; do
    if [ $((i % 6)) -eq 0 ]; then rounds=1500000; else rounds=150000; fi
    echo "$py $here/pbkdf2_bench.py --count 60 --rounds $rounds --tag b$i"
    i=$((i + 1))
done
