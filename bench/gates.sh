#!/bin/sh
# bench/gates.sh - the machine-checkable perf gates (sprint 10
# locked). Self-relative and syscall-count gates only; vs-oracle
# ratios are recorded tables (run-all.sh), not gates. Exit 1 on any
# gate failure.
#
# Gates:
#  G1 startup floor:  chopin <= GNU * 1.05 on one small file (guard)
#  G2 throttled-IO:   parallel <= serial * 1.05 under a 20 MB/s io
#                     throttle (rotational emulation; must-not-
#                     regress when bandwidth-bound) - needs sudo +
#                     systemd-run, else recorded SKIP
#  G3 FICLONE cache:  exactly 1 probe per non-CoW device pair
#  G4 spine walk:     CHOPIN_DEBUG_WALK over the kernel-tree fixture
#                     in < 200 ms (the serial spine is the kernel
#                     lane's floor)

set -u
. "$(dirname -- "$0")/lib.sh"

fails=0
gate() {   # gate <name> <pass|fail|skip> <detail>
    printf 'gate %-14s %s  %s\n' "$1" "$2" "$3"
    if [ "$2" = FAIL ]; then
        fails=$((fails + 1))
    fi
    return 0
}

# --- G1: startup floor -------------------------------------------------
fix=$(sh "$bench_root/bench/fixtures.sh" smallfile)
t_c=$(env LC_ALL=C hyperfine --runs 10 --warmup 3 --style basic \
    --prepare "rm -f $fix/dst" \
    --export-json "$resdir/g1c.json" \
    "$CHOPIN $fix/src/one $fix/dst" >/dev/null 2>&1 \
    && sed -n 's/.*"min": \([0-9.e-]*\).*/\1/p' "$resdir/g1c.json" | head -1)
t_g=$(env LC_ALL=C hyperfine --runs 10 --warmup 3 --style basic \
    --prepare "rm -f $fix/dst" \
    --export-json "$resdir/g1g.json" \
    "$GNU $fix/src/one $fix/dst" >/dev/null 2>&1 \
    && sed -n 's/.*"min": \([0-9.e-]*\).*/\1/p' "$resdir/g1g.json" | head -1)
if [ -n "$t_c" ] && [ -n "$t_g" ]; then
    ok=$(awk -v c="$t_c" -v g="$t_g" 'BEGIN { print (c <= g * 1.05) }')
    [ "$ok" = 1 ] && gate startup-floor PASS "chopin ${t_c}s gnu ${t_g}s" \
                  || gate startup-floor FAIL "chopin ${t_c}s gnu ${t_g}s"
else
    gate startup-floor FAIL "timing failed"
fi

# --- G2: throttled-IO guard (rotational emulation) --------------------
if have_sudo && command -v systemd-run >/dev/null 2>&1; then
    fix=$(sh "$bench_root/bench/fixtures.sh" swarm)
    blkdev=$(df --output=source "$fixdir" | tail -1)
    throttle="IOReadBandwidthMax=$blkdev 20M IOWriteBandwidthMax=$blkdev 20M"
    run_throttled() {
        # shellcheck disable=SC2086
        sudo systemd-run --scope --quiet --uid="$(id -u)" \
            -p "IOReadBandwidthMax=$blkdev 20M" \
            -p "IOWriteBandwidthMax=$blkdev 20M" \
            env LC_ALL=C "$@"
    }
    rm -rf "$fix/dst"
    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
    ts=$(date +%s.%N)
    run_throttled "$CHOPIN" -R "$fix/src" "$fix/dst" 2>/dev/null
    t_par=$(awk -v a="$ts" -v b="$(date +%s.%N)" 'BEGIN{print b-a}')
    rm -rf "$fix/dst"
    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
    ts=$(date +%s.%N)
    run_throttled env CHOPIN_PARALLEL_WORKERS=0 \
        "$CHOPIN" -R "$fix/src" "$fix/dst" 2>/dev/null
    t_ser=$(awk -v a="$ts" -v b="$(date +%s.%N)" 'BEGIN{print b-a}')
    rm -rf "$fix/dst"
    ok=$(awk -v p="$t_par" -v s="$t_ser" 'BEGIN { print (p <= s * 1.05) }')
    [ "$ok" = 1 ] && gate throttled-io PASS "par ${t_par}s ser ${t_ser}s" \
                  || gate throttled-io FAIL "par ${t_par}s ser ${t_ser}s"
else
    gate throttled-io SKIP "needs sudo + systemd-run"
fi

# --- G3: FICLONE cache = 1 probe per non-CoW pair ---------------------
noncow=""
for cand in /tmp /dev/shm; do
    [ -w "$cand" ] || continue
    cfs=$(stat -f -c %T "$cand" 2>/dev/null)
    case $cfs in tmpfs|ramfs) noncow="$cand"; break ;; esac
done
if [ -n "$noncow" ]; then
    g3s="$fixdir/g3src"
    g3d=$(mktemp -d "$noncow/chopin-g3.XXXXXX")
    rm -rf "$g3s"; mkdir -p "$g3s"
    i=0
    while [ $i -lt 100 ]; do
        head -c 1024 /dev/urandom > "$g3s/f$i"; i=$((i+1))
    done
    probes=$(env LC_ALL=C CHOPIN_DEBUG_STATS=1 \
        "$CHOPIN" -R "$g3s" "$g3d/out" 2>&1 \
        | sed -n 's/.*probes=\([0-9]*\).*/\1/p')
    rm -rf "$g3d" "$g3s"
    if [ "$probes" = 1 ]; then
        gate ficlone-cache PASS "1 probe for 100 files"
    else
        gate ficlone-cache FAIL "probes=$probes (want 1)"
    fi
else
    gate ficlone-cache SKIP "no tmpfs dest available"
fi

# --- G4: spine-traversal floor ----------------------------------------
fix=$(sh "$bench_root/bench/fixtures.sh" kernel-tree)
t_walk=$(env LC_ALL=C hyperfine --runs 5 --warmup 2 --style basic \
    --export-json "$resdir/g4.json" \
    "env CHOPIN_DEBUG_WALK=1 $CHOPIN -R $fix/src $fix/never" \
    >/dev/null 2>&1 \
    && sed -n 's/.*"min": \([0-9.e-]*\).*/\1/p' "$resdir/g4.json" | head -1)
if [ -n "$t_walk" ]; then
    ok=$(awk -v t="$t_walk" 'BEGIN { print (t < 0.200) }')
    [ "$ok" = 1 ] && gate spine-walk PASS "${t_walk}s over 65k files" \
                  || gate spine-walk FAIL "${t_walk}s (budget 0.200s)"
else
    gate spine-walk FAIL "walk timing failed"
fi

echo
[ "$fails" -eq 0 ] && echo "gates: ALL PASS" || echo "gates: $fails FAILED"
exit "$fails"
