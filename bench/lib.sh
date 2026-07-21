#!/bin/sh
# bench/lib.sh - shared methodology (overview s8, sprint 10 locked).
#
# Timing: hyperfine, min-of-N (N=BENCH_RUNS, default 3), LC_ALL=C.
# Cold lanes: sync + drop_caches in --prepare before EVERY timed run
# (wcp's own bench.sh procedure); they run only where passwordless
# sudo works and are recorded as such. After timing, the LAST run's
# result tree is manifest-diffed against the source - a fast copy
# that copied wrong is a failed lane, not a fast one.
#
# BENCH_SCALE=release uses the locked lane sizes (200k swarm, 1M
# empty, 4 GiB large...); the default dev scale is smaller for
# iteration. Tables that ship cite release scale only.

set -u

bench_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixdir="$bench_root/build/bench"
resdir="$bench_root/bench/results/$(hostname)-$(date +%Y%m%d)"
manifest="$bench_root/build/manifest"
scale="${BENCH_SCALE:-dev}"
runs="${BENCH_RUNS:-3}"

mkdir -p "$fixdir" "$resdir"

have_sudo() { sudo -n true 2>/dev/null; }

cold_prepare() {
    # Used inside hyperfine --prepare for cold lanes.
    echo "sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null"
}

machine_info() {
    {
        echo "host: $(hostname)"
        echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "kernel: $(uname -sr)"
        echo "cpu: $(sed -n 's/^model name[^:]*: //p' /proc/cpuinfo 2>/dev/null | head -1)"
        echo "cores: $(nproc 2>/dev/null || sysctl -n hw.ncpu)"
        echo "fs: $(stat -f -c %T "$fixdir" 2>/dev/null || stat -f "$fixdir" | sed -n 's/.*fstype //p')"
        echo "scale: $scale"
        echo "cold_capable: $(have_sudo && echo yes || echo no)"
    } > "$resdir/machine.txt"
}

# bench_lane <lane> <cold|warm> <src> <dst> <tool-name> <cmd...>
# Times "cmd" (which must copy src to dst), min-of-N, appends to the
# lane TSV, then manifest-verifies the surviving result tree.
bench_lane() {
    lane=$1 temp=$2 src=$3 dst=$4 tool=$5
    shift 5

    prep="rm -rf '$dst'"
    if [ "$temp" = cold ]; then
        have_sudo || { echo "  $lane/$tool: SKIP (cold needs sudo)"; return 0; }
        prep="$prep; $(cold_prepare)"
    fi

    json="$resdir/$lane.$tool.json"
    if ! env LC_ALL=C hyperfine --runs "$runs" --prepare "$prep" \
        --export-json "$json" --style basic \
        "$(printf '%s ' "$@")" >/dev/null 2>&1; then
        echo "  $lane/$tool: FAILED (command errored)"
        echo "$lane	$tool	FAIL	$temp" >> "$resdir/summary.tsv"
        return 1
    fi

    best=$(sed -n 's/.*"min": \([0-9.e-]*\).*/\1/p' "$json" | head -1)
    echo "$lane	$tool	$best	$temp" >> "$resdir/summary.tsv"
    echo "  $lane/$tool: ${best}s ($temp, min of $runs)"

    # Correctness: the surviving tree must match the source.
    if [ -x "$manifest" ] && [ -d "$dst" ]; then
        base=$(basename "$src")
        cmproot="$dst"
        [ -d "$dst/$base" ] && cmproot="$dst/$base"
        if ! "$manifest" "$src" > "$resdir/.m.src" 2>/dev/null \
            || ! "$manifest" "$cmproot" > "$resdir/.m.dst" 2>/dev/null \
            || ! cmp -s "$resdir/.m.src" "$resdir/.m.dst"; then
            echo "  $lane/$tool: RESULT-TREE MISMATCH - lane invalid" >&2
            echo "$lane	$tool	TREE-MISMATCH	$temp" >> "$resdir/summary.tsv"
            return 1
        fi
    fi
    return 0
}

# Tool table: name + command prefix. Rivals bench only when present.
CHOPIN="$bench_root/chopin"
GNU="$bench_root/build/gnu-cp/src/cp"
FCP="${FCP:-$HOME/.cargo/bin/fcp}"
XCP="${XCP:-$HOME/.cargo/bin/xcp}"
WCP="${WCP:-$bench_root/build/wcp-src/build/wcp}"
