#!/bin/sh
# bench/run-smoke.sh - the check-tier perf sanity lane (sprint 10,
# CLAUDE.md promise). Self-relative only: chopin-parallel vs
# chopin-scalar on a small swarm, seconds-scale, no rivals, no
# oracle. Guards against a catastrophic perf regression landing
# silently; real numbers live in run-all.sh.
#
# PASS: both configurations complete, trees verify, and parallel is
# not slower than 3x scalar (generous - CI boxes are noisy; the real
# gates run in bench/gates.sh on bench machines).

set -u
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

command -v hyperfine >/dev/null 2>&1 || {
    echo "perf-smoke: no hyperfine; skipping (77)"; exit 77; }
[ -x "$root/chopin" ] || { echo "perf-smoke: build first"; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/chopin-psmoke.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

mkdir -p "$work/src"
i=0
while [ $i -lt 32 ]; do mkdir "$work/src/d$i"; i=$((i+1)); done
head -c 4096 /dev/urandom > "$work/.blk"
i=0
while [ $i -lt 2000 ]; do
    cp "$work/.blk" "$work/src/d$((i % 32))/f$i"
    i=$((i+1))
done

t_par=$(env LC_ALL=C hyperfine --runs 3 --style basic \
    --prepare "rm -rf $work/dst" \
    --export-json "$work/par.json" \
    "env CHOPIN_PARALLEL_MIN=1 $root/chopin -R $work/src $work/dst" \
    >/dev/null 2>&1 && sed -n 's/.*"min": \([0-9.e-]*\).*/\1/p' \
    "$work/par.json" | head -1)
diff -r "$work/src" "$work/dst" >/dev/null || {
    echo "perf-smoke: parallel result tree differs"; exit 1; }

t_ser=$(env LC_ALL=C hyperfine --runs 3 --style basic \
    --prepare "rm -rf $work/dst" \
    --export-json "$work/ser.json" \
    "env CHOPIN_PARALLEL_WORKERS=0 $root/chopin -R $work/src $work/dst" \
    >/dev/null 2>&1 && sed -n 's/.*"min": \([0-9.e-]*\).*/\1/p' \
    "$work/ser.json" | head -1)
diff -r "$work/src" "$work/dst" >/dev/null || {
    echo "perf-smoke: serial result tree differs"; exit 1; }

[ -n "$t_par" ] && [ -n "$t_ser" ] || {
    echo "perf-smoke: timing failed"; exit 1; }

bad=$(awk -v p="$t_par" -v s="$t_ser" 'BEGIN { print (p > 3 * s) }')
if [ "$bad" = 1 ]; then
    echo "perf-smoke: FAIL parallel ${t_par}s > 3x scalar ${t_ser}s"
    exit 1
fi
echo "perf-smoke: ok (parallel ${t_par}s, scalar ${t_ser}s)"
