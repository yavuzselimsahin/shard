#!/bin/sh
# Turns a text file into a C string constant, so the web UI can ship inside
# the binary. Generated output is committed: building shard never needs this.
set -e

file=$1
name=$2

if [ -z "$file" ] || [ -z "$name" ]; then
    echo "usage: embed.sh <file> <symbol>" >&2
    exit 1
fi

printf '/* Generated from %s by tools/embed.sh — do not edit. */\n' "$file"
printf 'static const char %s[] =\n' "$name"
sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$/\\n"/' "$file"
printf ';\n'
