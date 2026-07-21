#!/bin/sh
# bench/syscall-budget.sh - strace -c profiles vs GNU (sprint 10
# locked; Linux only, clean skip elsewhere). Budgets are asserted on
# SERIAL chopin: pool threads add clone/futex noise that says nothing
# about per-file fidelity; a parallel profile is recorded alongside
# as a report. Per-file and per-invocation costs are budgeted
# separately (GNU's per-invocation O_PATH target-dir probe,
# gnu-cp-analysis.md 9.1).
#
# Budget (bench/budget.txt records the disposition):
#  B1 minimal 2-arg fresh-dst copy: chopin total <= GNU total.
#  B2 per-file marginal cost (swarm-33 minus swarm-1, /32):
#     chopin <= GNU + allowlist (currently empty).
#  B3 FICLONE cache: non-CoW 32-file copy performs at most 1
#     clone-class ioctl (GNU pays one per file).

set -u
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

case $(uname -s) in Linux) ;; *)
    echo "syscall-budget: Linux only; skipping (77)"; exit 77 ;; esac
command -v strace >/dev/null 2>&1 || {
    echo "syscall-budget: no strace; skipping (77)"; exit 77; }

CHOPIN="$root/chopin"
GNU="$root/build/gnu-cp/src/cp"
[ -x "$GNU" ] || { echo "syscall-budget: no pinned oracle (77)"; exit 77; }

work=$(mktemp -d "${TMPDIR:-/tmp}/chopin-sbudget.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT INT TERM
res="$root/bench/results"
mkdir -p "$res"

total_calls() {   # total_calls <out> <cmd...>
    out=$1; shift
    strace -f -c -o "$out.raw" "$@" >/dev/null 2>&1
    awk '/^100\.00/ { print $4; found=1 } END { if (!found) print 0 }' \
        "$out.raw"
}

fails=0

# --- B1: minimal 2-arg fresh-dst -------------------------------------
head -c 4096 /dev/urandom > "$work/one"
c1=$(total_calls "$work/c1" env LC_ALL=C CHOPIN_PARALLEL_WORKERS=0 \
    "$CHOPIN" "$work/one" "$work/dc"; rm -f "$work/dc")
g1=$(total_calls "$work/g1" env LC_ALL=C "$GNU" "$work/one" "$work/dg")
rm -f "$work/dg"
if [ "$c1" -le "$g1" ]; then
    echo "B1 minimal-2arg   PASS  chopin=$c1 gnu=$g1"
else
    echo "B1 minimal-2arg   FAIL  chopin=$c1 gnu=$g1"; fails=$((fails+1))
fi

# --- B2: per-file marginal (swarm-33 vs swarm-1) ----------------------
mkdir -p "$work/s33" "$work/s1"
head -c 1024 /dev/urandom > "$work/s1/f0"
i=0
while [ $i -lt 33 ]; do cp "$work/s1/f0" "$work/s33/f$i"; i=$((i+1)); done
c33=$(total_calls "$work/c33" env LC_ALL=C CHOPIN_PARALLEL_WORKERS=0 \
    "$CHOPIN" -R "$work/s33" "$work/dc33"; rm -rf "$work/dc33")
cs1=$(total_calls "$work/cs1" env LC_ALL=C CHOPIN_PARALLEL_WORKERS=0 \
    "$CHOPIN" -R "$work/s1" "$work/dcs1"; rm -rf "$work/dcs1")
g33=$(total_calls "$work/g33" env LC_ALL=C "$GNU" -R "$work/s33" "$work/dg33")
rm -rf "$work/dg33"
gs1=$(total_calls "$work/gs1" env LC_ALL=C "$GNU" -R "$work/s1" "$work/dgs1")
rm -rf "$work/dgs1"
cpf=$(( (c33 - cs1) / 32 ))
gpf=$(( (g33 - gs1) / 32 ))
if [ "$cpf" -le "$gpf" ]; then
    echo "B2 per-file       PASS  chopin=$cpf gnu=$gpf (marginal calls/file)"
else
    echo "B2 per-file       FAIL  chopin=$cpf gnu=$gpf"; fails=$((fails+1))
fi

# --- B3: FICLONE-cache ioctl count on a non-CoW pair ------------------
noncow=""
for cand in /dev/shm /tmp; do
    [ -w "$cand" ] || continue
    case $(stat -f -c %T "$cand" 2>/dev/null) in
    tmpfs|ramfs) noncow=$cand; break ;; esac
done
if [ -n "$noncow" ]; then
    g3d=$(mktemp -d "$noncow/chopin-b3.XXXXXX")
    strace -f -e trace=ioctl -o "$work/b3.c" env LC_ALL=C \
        CHOPIN_PARALLEL_WORKERS=0 \
        "$CHOPIN" -R "$work/s33" "$g3d/out" >/dev/null 2>&1
    rm -rf "$g3d/out"
    ci=$(grep -c FICLONE "$work/b3.c" || true)
    strace -f -e trace=ioctl -o "$work/b3.g" env LC_ALL=C \
        "$GNU" -R "$work/s33" "$g3d/out" >/dev/null 2>&1
    gi=$(grep -c FICLONE "$work/b3.g" || true)
    rm -rf "$g3d"
    if [ "$ci" -le 1 ]; then
        echo "B3 ficlone-cache  PASS  chopin=$ci ioctls gnu=$gi (33 files)"
    else
        echo "B3 ficlone-cache  FAIL  chopin=$ci ioctls"; fails=$((fails+1))
    fi
else
    echo "B3 ficlone-cache  SKIP  no tmpfs dest"
fi

# Record the full tables for the ledger.
cp "$work/c1.raw" "$res/syscalls-minimal-chopin.txt" 2>/dev/null || true
cp "$work/g1.raw" "$res/syscalls-minimal-gnu.txt" 2>/dev/null || true

[ "$fails" -eq 0 ] && echo "syscall-budget: ALL PASS" \
                   || echo "syscall-budget: $fails FAILED"
exit "$fails"
