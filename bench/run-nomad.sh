#!/bin/sh
# bench/run-nomad.sh - drive the nomad-1 (Apple silicon) matrix from
# the dev box over Tailscale.
#
# The run is DETACHED on the remote with nohup: nomad-1 is a laptop on
# a relayed link, and a sleep or a dropped tunnel SIGHUPs anything
# attached to the ssh session (it killed one full matrix mid-run).
# Launch, then poll for the done-stamp; the run survives disconnects.
#
#   sh bench/run-nomad.sh start   # ship tree, build, launch detached
#   sh bench/run-nomad.sh status  # rows so far / done?
#   sh bench/run-nomad.sh fetch   # copy results back into bench/results

set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HOST=${NOMAD_HOST:-mfwolffe@100.123.81.66}
RDIR=${NOMAD_DIR:-chopin-bench}
SH="/bin/sh -lc"
PATHS="export PATH=/opt/homebrew/bin:\$HOME/.cargo/bin:/usr/bin:/bin:/usr/sbin:/sbin"

case "${1:-status}" in
start)
    echo "== shipping tree =="
    git -C "$root" archive HEAD | ssh "$HOST" "$SH \"rm -rf ~/$RDIR && mkdir -p ~/$RDIR && tar -x -C ~/$RDIR\""
    # Uncommitted work in progress must not silently differ from the
    # tables; ship the tracked tree only and say so.
    if [ -n "$(git -C "$root" status --porcelain -- src bench)" ]; then
        echo "note: uncommitted src/bench changes are NOT in this run" >&2
    fi
    echo "== building (oracle reused if present) =="
    ssh "$HOST" "$SH \"$PATHS; cd ~/$RDIR && ./configure >/dev/null && gmake -j6 2>&1 | grep -E 'error:' | head -3; test -x build/gnu-cp/src/cp || sh scripts/build-gnu-cp.sh >/dev/null 2>&1; ./build/gnu-cp/src/cp --version | head -1\""
    echo "== launching detached matrix =="
    ssh "$HOST" "$SH \"$PATHS; cd ~/$RDIR && rm -f bench.done && nohup sh -c 'BENCH_SCALE=\${BENCH_SCALE:-release} BENCH_RUNS=3 GNU=./build/gnu-cp/src/cp BENCH_TOOLS=\\\"chopin chopin-serial gnu fcp xcp\\\" sh bench/run-all.sh > bench.log 2>&1; echo \\\$? > bench.done' >/dev/null 2>&1 &\""
    echo "launched; poll with: sh bench/run-nomad.sh status"
    ;;
status)
    ssh -o ConnectTimeout=20 "$HOST" "$SH \"cd ~/$RDIR 2>/dev/null && { echo rows=\\\$(cat bench/results/*/summary.tsv 2>/dev/null | wc -l); if [ -f bench.done ]; then echo done rc=\\\$(cat bench.done); else echo running; tail -2 bench.log 2>/dev/null; fi; }\"" \
        || echo "nomad unreachable (asleep or off-tailnet)"
    ;;
fetch)
    d=$(ssh "$HOST" "$SH \"ls -d ~/$RDIR/bench/results/*/ | tail -1\"")
    mkdir -p "$root/bench/results"
    scp -q -r "$HOST:$d" "$root/bench/results/" && echo "fetched into bench/results/$(basename "$d")"
    ;;
*)
    echo "usage: run-nomad.sh {start|status|fetch}" >&2; exit 2 ;;
esac
