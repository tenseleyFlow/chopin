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
    echo "== shipping tree (build/ preserved) =="
    # Keep build/: it holds the pinned oracle and tens of GiB of
    # seeded fixtures that cost far more to rebuild than they cost to
    # keep. Only tracked sources are replaced. (A blanket rm -rf also
    # failed mid-flight once on APFS and left a source-less tree.)
    ssh "$HOST" "$SH \"mkdir -p ~/$RDIR && cd ~/$RDIR && find . -maxdepth 1 -mindepth 1 ! -name build -exec rm -rf {} + \""
    git -C "$root" archive HEAD | ssh "$HOST" "$SH \"tar -x -C ~/$RDIR\""
    # Uncommitted work in progress must not silently differ from the
    # tables; ship the tracked tree only and say so.
    if [ -n "$(git -C "$root" status --porcelain -- src bench)" ]; then
        echo "note: uncommitted src/bench changes are NOT in this run" >&2
    fi
    echo "== building (oracle reused if present) =="
    ssh "$HOST" "$SH \"$PATHS; cd ~/$RDIR && ./configure >/dev/null && gmake -j6 2>&1 | grep -E 'error:' | head -3; test -x build/gnu-cp/src/cp || sh scripts/build-gnu-cp.sh >/dev/null 2>&1; ./build/gnu-cp/src/cp --version | head -1\""
    echo "== launching detached A/B sweep =="
    # bench/ab.sh, not run-all.sh: the matrix runs all reps of one tool
    # then the other, which biases whichever runs second by ~10% on
    # warm lanes. ab.sh interleaves and times with nanosecond clocks.
    ssh "$HOST" "$SH \"$PATHS; cd ~/$RDIR && rm -f bench.done ab.log && nohup sh -c 'for L in swarm kernel-tree large-nocow sparse-nocow reflink hardlink-farm metadata-heavy swarm-empty smallfile; do sh bench/ab.sh \\\$L 6 warm 2>&1 | tail -1; done > ab.log 2>&1; echo \\\$? > bench.done' >/dev/null 2>&1 &\""
    echo "launched; poll with: sh bench/run-nomad.sh status"
    ;;
status)
    ssh -o ConnectTimeout=20 "$HOST" "$SH \"cd ~/$RDIR 2>/dev/null && { if [ -f bench.done ]; then echo \\\"DONE rc=\\\$(cat bench.done)\\\"; else echo RUNNING; fi; cat ab.log 2>/dev/null; }\"" \
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
