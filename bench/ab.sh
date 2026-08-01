#!/bin/sh
# bench/ab.sh - interleaved A/B comparison for one lane.
#
# Why this exists: hyperfine runs every repetition of command A, then
# every repetition of B. Over a long matrix on a machine whose state
# drifts (page-cache pressure, fixture churn, thermals), that biases
# whichever tool ran later - it reported large-nocow-warm as chopin
# 0.86x GNU where an interleaved run shows a tie. Interleaving
# alternates A,B,A,B... so drift hits both tools equally, and min-of-N
# then reflects the tool, not the moment.
#
# Usage: sh bench/ab.sh <lane> <reps> [cold]
# Prints: per-rep times, then min for each tool and the ratio.

set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
lane=${1:?usage: ab.sh <lane> <reps> [cold]}
reps=${2:-5}
temp=${3:-warm}

CHOPIN="$root/chopin"
# B defaults to the pinned oracle; AB_B=fcp|xcp|wcp compares against a
# locally re-baselined rival on the same fixture and methodology.
case ${AB_B:-gnu} in
gnu) GNU="$root/build/gnu-cp/src/cp"; B_NAME=gnu ;;
fcp) GNU="${FCP:-$HOME/.cargo/bin/fcp}"; B_NAME=fcp ;;
xcp) GNU="${XCP:-$HOME/.cargo/bin/xcp}"; B_NAME=xcp ;;
wcp) GNU="$root/build/wcp-src/build/wcp"; B_NAME=wcp ;;
*)   echo "ab: unknown AB_B" >&2; exit 2 ;;
esac
[ -x "$GNU" ] || { echo "ab: $B_NAME not available"; exit 77; }

case $lane in
large-nocow)  fixlane=large-single; args="-R --reflink=never" ;;
sparse-nocow) fixlane=sparse;       args="-R --reflink=never" ;;
metadata-heavy|hardlink-farm) fixlane=$lane; args="-a" ;;
*)            fixlane=$lane;        args="-R" ;;
esac

fix=$(sh "$root/bench/fixtures.sh" "$fixlane")
src="$fix/src"; dst="$fix/ab-dst"

drop() {
    [ "$temp" = cold ] || return 0
    [ "$(uname -s)" = Linux ] || return 0
    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1 || true
}

# /usr/bin/time -f %e reports 10ms granularity, which is 5% of a
# 0.2s lane - too coarse to distinguish 0.85x from parity. Time with
# nanosecond clocks instead.
one() {  # one <tool-path> [args-override]
    rm -rf "$dst"; drop
    # ${2-...} not ${2:-...}: rivals legitimately take NO flags, and
    # the colon form would substitute cp's flags for an empty
    # override - which made fcp error out in 1ms and score a 500x
    # "win" until this was caught.
    o_args=${2-$args}
    o_t0=$(date +%s.%N)
    env LC_ALL=C $1 $o_args "$src" "$dst" >/dev/null 2>&1
    o_rc=$?
    o_t1=$(date +%s.%N)
    [ "$o_rc" = 0 ] || { echo "ab: $1 exited $o_rc" >&2; return 1; }
    awk -v a="$o_t0" -v b="$o_t1" 'BEGIN{printf "%.4f", b - a}'
}

# Rivals do not share cp's flag surface: fcp takes no -R, xcp uses -r,
# and neither implements -a. Give each its closest equivalent.
b_args=$args
case $B_NAME in
fcp) b_args=$(echo "$args" | sed -e 's/-R//' -e 's/-a//' -e 's/--reflink=never//') ;;
xcp) b_args=$(echo "$args" | sed -e 's/-R/-r/' -e 's/-a/-r/' -e 's/--reflink=never//') ;;
wcp) b_args=$(echo "$args" | sed -e 's/-R//' -e 's/-a//' -e 's/--reflink=never//') ;;
esac

# Order alternates per rep (ABBA): running one tool first every time
# lets the second tool read a source the first just pulled into page
# cache. That bias is worth ~10% on warm bulk lanes - it is how this
# script first "found" a large-file regression that does not exist.
cmin=""; gmin=""
i=1
while [ "$i" -le "$reps" ]; do
    if [ $((i % 2)) -eq 1 ]; then
        c=$(one "$CHOPIN"); g=$(one "$GNU" "$b_args")
    else
        g=$(one "$GNU" "$b_args"); c=$(one "$CHOPIN")
    fi
    printf '  rep %d: chopin %s  %s %s\n' "$i" "$c" "$B_NAME" "$g"
    cmin=$(awk -v a="$cmin" -v b="$c" 'BEGIN{print (a=="" || b+0<a+0) ? b : a}')
    gmin=$(awk -v a="$gmin" -v b="$g" 'BEGIN{print (a=="" || b+0<a+0) ? b : a}')
    i=$((i + 1))
done
rm -rf "$dst"

awk -v l="$lane" -v t="$temp" -v c="$cmin" -v g="$gmin" -v b="$B_NAME" 'BEGIN {
    printf "%-18s %-5s chopin %8.4f  %-4s %8.4f  -> %.2fx %s\n",
        l, t, c, b, g, g / c, (g / c >= 1.0 ? "" : "(chopin SLOWER)")
}'
