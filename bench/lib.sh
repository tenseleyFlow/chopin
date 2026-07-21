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
    # bl_-prefixed: POSIX sh has no locals and the caller's loop
    # variables (lane, tool...) must survive this call.
    bl_lane=$1 bl_temp=$2 bl_src=$3 bl_dst=$4 bl_tool=$5
    shift 5

    bl_prep="rm -rf '$bl_dst'"
    bl_warm=""
    if [ "$bl_temp" = cold ]; then
        have_sudo || {
            echo "  $bl_lane/$bl_tool: SKIP (cold needs sudo)"; return 0; }
        bl_prep="$bl_prep; $(cold_prepare)"
    else
        # Micro-lanes without warmup report first-run noise as min.
        bl_warm="--warmup 2"
    fi

    bl_json="$resdir/$bl_lane.$bl_tool.json"
    # shellcheck disable=SC2086
    if ! env LC_ALL=C hyperfine --runs "$runs" $bl_warm \
        --prepare "$bl_prep" \
        --export-json "$bl_json" --style basic \
        "$(printf '%s ' "$@")" >/dev/null 2>&1; then
        echo "  $bl_lane/$bl_tool: FAILED (command errored)"
        echo "$bl_lane	$bl_tool	FAIL	$bl_temp" >> "$resdir/summary.tsv"
        return 1
    fi

    bl_best=$(sed -n 's/.*"min": \([0-9.e-]*\).*/\1/p' "$bl_json" | head -1)
    echo "$bl_lane	$bl_tool	$bl_best	$bl_temp" >> "$resdir/summary.tsv"
    echo "  $bl_lane/$bl_tool: ${bl_best}s ($bl_temp, min of $runs)"

    # Correctness: the surviving tree must match the source.
    if [ -x "$manifest" ] && [ -d "$bl_dst" ]; then
        bl_base=$(basename "$bl_src")
        bl_cmproot="$bl_dst"
        [ -d "$bl_dst/$bl_base" ] && bl_cmproot="$bl_dst/$bl_base"
        if ! "$manifest" "$bl_src" > "$resdir/.m.src" 2>/dev/null \
            || ! "$manifest" "$bl_cmproot" > "$resdir/.m.dst" 2>/dev/null \
            || ! cmp -s "$resdir/.m.src" "$resdir/.m.dst"; then
            echo "  $bl_lane/$bl_tool: RESULT-TREE MISMATCH - lane invalid" >&2
            echo "$bl_lane	$bl_tool	TREE-MISMATCH	$bl_temp" >> "$resdir/summary.tsv"
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
