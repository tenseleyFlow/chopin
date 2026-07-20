#!/bin/sh
# chopin golden differential tier - harness skeleton (sprint 00C).
#
# Twin sandboxes built from one seed, oracle and UUT run under env -i
# pinning, result trees compared via the manifest tool, stdout/stderr
# byte-compared (normprog'd; SORTED mode sorts both streams for
# multi-entry directory cases - GNU's within-directory order is inode
# order, fs-dependent), exit codes compared.
#
# DESTRUCTIVE-TOOL SAFETY (never weaken to make a case pass):
#   - refuses to run as root;
#   - every path the tools touch must realpath-resolve under a
#     REGISTERED containment root (asserted before any comparison);
#   - harness scratch (capture files, manifests) lives OUTSIDE both
#     compared trees, pre-created before the tools run (the ..-size
#     fuzz lesson: capture files that appear mid-run can enter the
#     comparison surface).
#
# Case machinery is live but the case matrix arrives with sprint 02;
# this sprint runs the oracle-vs-oracle self-test phase.
#
# Exit: 0 green, 1 failures, 77 skip (no oracle).
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
cd "$root"

# --- safety prologue -------------------------------------------------

# Refusal = the destructive tier never executes as root. Exit 77 (skip)
# rather than 1 so root-only CI VMs (vmactions FreeBSD) keep their
# build+unit signal; alpine runs non-root precisely so this tier DOES
# execute there (family root-skip fix).
if [ "$(id -u)" = 0 ]; then
    echo "golden: SKIPPED - refusing to run as root (destructive tool harness)" >&2
    exit 77
fi

work="$root/tests/.work/golden"
chmod -R u+rwx "$work" 2>/dev/null || true
rm -rf "$work"
mkdir -p "$work"
workreal=$(CDPATH= cd -- "$work" && pwd -P)

# Registered containment roots (sprint 05 adds the probed EXDEV root).
containment_roots="$workreal"

cleanup() {
    chmod -R u+rwx "$work" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

# Assert DIR realpath-resolves under a registered root.
assert_contained() {
    d="$1"
    dr=$(CDPATH= cd -- "$d" 2>/dev/null && pwd -P) || {
        echo "golden: CONTAINMENT: cannot resolve $d" >&2
        exit 1
    }
    for r in $containment_roots; do
        case "$dr/" in
        "$r"/*) return 0 ;;
        esac
    done
    echo "golden: CONTAINMENT VIOLATION: $dr outside registered roots" >&2
    exit 1
}

# --- tools -----------------------------------------------------------

uut="${CHOPIN_UUT:-$root/chopin}"
[ -x "$uut" ] || {
    echo "golden: UUT not built ($uut); run make first" >&2
    exit 1
}

oracle=$(sh scripts/find-gnu-cp.sh) || {
    echo "golden: no GNU cp oracle found; skipping tier (77)" >&2
    echo "  (sh scripts/build-gnu-cp.sh builds the pinned one)" >&2
    trap - EXIT; cleanup
    exit 77
}
oracle_ver=$("$oracle" --version | sed -n 1p)
case "$oracle_ver" in
*" 9.11") PARITY_ACTIVE=1 ;;
*)
    PARITY_ACTIVE=0
    echo "golden: oracle is not the 9.11 pin ($oracle_ver);" >&2
    echo "  self-tests run, parity cases skip" >&2
    ;;
esac

# Manifest tool (built by the unit tier; rebuild if absent/stale).
manifest="$root/build/manifest"
if [ ! -x "$manifest" ] || [ "tests/manifest.c" -nt "$manifest" ]; then
    cc_bin=$(sed -n 's/^CC ?= //p' config.mk)
    extra=$(sed -n 's/^EXTRA_CPPFLAGS = //p' config.mk)
    "$cc_bin" -std=c11 -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 $extra \
        -I. -Wall -Wextra -Werror -o "$manifest" tests/manifest.c || {
        echo "golden: cannot build manifest tool" >&2
        exit 1
    }
fi

# --- env -i pinning --------------------------------------------------

# run_pinned LC_ALL workdir tool [args...]; stdout/stderr already
# redirected by the caller. CHOPIN_PARALLEL_MIN/_WORKERS forwarded so
# the tsan tier reaches the UUT (liszt run.sh:111 pattern).
run_pinned() {
    rp_lc="$1"; rp_wd="$2"; rp_tool="$3"
    shift 3
    (
        cd "$rp_wd" || exit 126
        umask 022
        env -i \
            PATH="/usr/bin:/bin" \
            HOME="$rp_wd" \
            LC_ALL="$rp_lc" \
            LANGUAGE=C \
            TZ=UTC0 \
            ${CHOPIN_PARALLEL_MIN:+CHOPIN_PARALLEL_MIN="$CHOPIN_PARALLEL_MIN"} \
            ${CHOPIN_PARALLEL_WORKERS:+CHOPIN_PARALLEL_WORKERS="$CHOPIN_PARALLEL_WORKERS"} \
            "$rp_tool" "$@"
    )
}

# Normalize the leading program token of diagnostic lines so oracle
# ("cp: ...") and UUT ("chopin: ..."/"cpn: ...") stderr compare.
normprog() {
    sed -e 's/^cp:/PROG:/' -e 's/^chopin:/PROG:/' -e 's/^cpn:/PROG:/'
}

# --- twin sandboxes --------------------------------------------------

# Seed functions take the sandbox dir and must create src/ and dst/
# inside it. Both sandboxes are built by the SAME function under the
# same umask - byte-identical starting states.
seed_smoke() {
    s="$1"
    umask 022
    mkdir -p "$s/src/sub" "$s/dst"
    printf 'alpha\n' > "$s/src/a.txt"
    printf 'beta\n' > "$s/src/sub/b.txt"
    ln "$s/src/a.txt" "$s/src/hard.txt"
    ln -s a.txt "$s/src/link"
    : > "$s/src/sub/with space"
    printf 'x' > "$s/src/sparse.bin"
    truncate -s 262144 "$s/src/sparse.bin" 2>/dev/null || true
    printf 'old\n' > "$s/dst/a.txt"
}

mkpair() {
    mp_case="$1"; mp_seed="$2"
    rm -rf "$work/$mp_case"
    mkdir -p "$work/$mp_case/A" "$work/$mp_case/B"
    "$mp_seed" "$work/$mp_case/A"
    "$mp_seed" "$work/$mp_case/B"
    assert_contained "$work/$mp_case/A"
    assert_contained "$work/$mp_case/B"
}

# --- case runner -----------------------------------------------------

cases=0
passed=0
failed=0
skipped=0

fail_case() {
    fc_tag="$1"; fc_why="$2"
    echo "FAIL $fc_tag: $fc_why"
    failed=$((failed + 1))
}

# run_case TAG desc MODE lc rc -- <args with SRC/DST tokens>
#   MODE  ORDERED (byte order) | SORTED (stdout+stderr through sort)
#   lc    LC_ALL value for both tools
#   rc    expected exit status, or '-' to only require agreement
#   SRC/DST expand to the per-tool sandbox src/dst paths.
#   Seed via CASE_SEED (defaults to seed_smoke).
run_case() {
    tag="$1"; desc="$2"; mode="$3"; lc="$4"; wantrc="$5"
    shift 5
    [ "$1" = "--" ] || { echo "run_case $tag: missing --" >&2; exit 1; }
    shift

    cases=$((cases + 1))
    if [ "$PARITY_ACTIVE" != 1 ]; then
        skipped=$((skipped + 1))
        return 0
    fi

    seed="${CASE_SEED:-seed_smoke}"
    mkpair "case-$tag" "$seed"
    cdir="$work/case-$tag"

    # Pre-create every capture file before either tool runs.
    for f in A.out A.err B.out B.err A.rc B.rc A.man B.man; do
        : > "$cdir/$f"
    done

    # Save the operand template: "$@" is rebuilt per side below, so the
    # original must survive side A (newline-joined; case authors keep
    # newlines out of operands - the fuzz tier has its own runner).
    tmpl=""
    for a in "$@"; do
        tmpl="$tmpl$a
"
    done

    for side in A B; do
        if [ "$side" = A ]; then tool="$oracle"; else tool="$uut"; fi
        sb="$cdir/$side"
        oldifs=$IFS; IFS='
'
        set -f
        # shellcheck disable=SC2086
        set -- $tmpl
        set +f
        IFS=$oldifs
        # Rotate-expand SRC/DST tokens in place. Tokens expand RELATIVE
        # (run_pinned cd's into the sandbox): -v/diagnostic lines quote
        # operands as given, so absolute per-sandbox paths would never
        # byte-match between sides A and B.
        n=$#
        while [ "$n" -gt 0 ]; do
            a="$1"; shift
            case "$a" in
            SRC) a="src" ;;
            SRC/*) a="src/${a#SRC/}" ;;
            DST) a="dst" ;;
            DST/*) a="dst/${a#DST/}" ;;
            esac
            set -- "$@" "$a"
            n=$((n - 1))
        done
        run_pinned "$lc" "$sb" "$tool" "$@" \
            > "$cdir/$side.out" 2> "$cdir/$side.err"
        echo $? > "$cdir/$side.rc"
        assert_contained "$sb"
        "$manifest" ${MANIFEST_FLAGS:-} "$sb/dst" > "$cdir/$side.man" \
            || fail_case "$tag" "manifest of side $side failed"
    done

    rca=$(cat "$cdir/A.rc"); rcb=$(cat "$cdir/B.rc")
    ok=1
    if [ "$rca" != "$rcb" ]; then
        fail_case "$tag" "rc: oracle=$rca uut=$rcb"; ok=0
    elif [ "$wantrc" != "-" ] && [ "$rca" != "$wantrc" ]; then
        fail_case "$tag" "rc: both=$rca expected=$wantrc"; ok=0
    fi
    if ! cmp -s "$cdir/A.man" "$cdir/B.man"; then
        fail_case "$tag" "result trees differ"
        diff "$cdir/A.man" "$cdir/B.man" | head -10
        ok=0
    fi
    if [ "$mode" = SORTED ]; then
        sort "$cdir/A.out" > "$cdir/A.out.s"; sort "$cdir/B.out" > "$cdir/B.out.s"
        sort "$cdir/A.err" | normprog > "$cdir/A.err.s"
        sort "$cdir/B.err" | normprog > "$cdir/B.err.s"
    else
        cp "$cdir/A.out" "$cdir/A.out.s"; cp "$cdir/B.out" "$cdir/B.out.s"
        normprog < "$cdir/A.err" > "$cdir/A.err.s"
        normprog < "$cdir/B.err" > "$cdir/B.err.s"
    fi
    if ! cmp -s "$cdir/A.out.s" "$cdir/B.out.s"; then
        fail_case "$tag" "stdout differs"
        diff "$cdir/A.out.s" "$cdir/B.out.s" | head -10
        ok=0
    fi
    if ! cmp -s "$cdir/A.err.s" "$cdir/B.err.s"; then
        fail_case "$tag" "stderr differs"
        diff "$cdir/A.err.s" "$cdir/B.err.s" | head -10
        ok=0
    fi
    if [ "$ok" = 1 ]; then
        passed=$((passed + 1))
        rm -rf "$cdir"
    else
        echo "  artifacts kept: $cdir"
    fi
}

# --- self-test phase: oracle vs oracle -------------------------------
# Proves sandbox building, pinning, capture, manifest comparison and
# containment before chopin can copy at all. Runs twice (determinism).

selftest_round() {
    st_round="$1"
    mkpair "self-$st_round" seed_smoke
    cdir="$work/self-$st_round"
    for f in A.out A.err B.out B.err A.man B.man; do
        : > "$cdir/$f"
    done
    for side in A B; do
        sb="$cdir/$side"
        run_pinned C "$sb" "$oracle" -R "$sb/src" "$sb/dst" \
            > "$cdir/$side.out" 2> "$cdir/$side.err" || true
        assert_contained "$sb"
        "$manifest" "$sb/dst" > "$cdir/$side.man"
    done
    if ! cmp -s "$cdir/A.man" "$cdir/B.man"; then
        echo "golden: SELF-TEST FAILED (round $st_round): twin oracle runs differ" >&2
        diff "$cdir/A.man" "$cdir/B.man" | head -10 >&2
        exit 1
    fi
    if ! cmp -s "$cdir/A.err" "$cdir/B.err"; then
        echo "golden: SELF-TEST FAILED (round $st_round): oracle stderr differs" >&2
        exit 1
    fi
    rm -rf "$cdir"
}

selftest_round 1
selftest_round 2
echo "golden: oracle self-test passed (2 rounds, oracle: $oracle_ver)"

# run_case machinery self-test: the oracle stands in as UUT, so every
# comparison must come back identical through the full case path
# (token expansion, capture, manifest, normprog, rc).
real_uut="$uut"
uut="$oracle"
run_case self-runcase "run_case plumbing, oracle as UUT" ORDERED C 0 \
    -- -R SRC/sub DST/subcopy
run_case self-runcase-v "-v stream capture, oracle as UUT" SORTED C 0 \
    -- -Rv SRC/sub DST/subcopy
uut="$real_uut"
if [ "$failed" -gt 0 ]; then
    echo "golden: run_case SELF-TEST FAILED" >&2
    exit 1
fi
selftest_cases=$cases
echo "golden: run_case self-test passed ($selftest_cases cases)"

# --- case matrix (arrives with sprint 02) ----------------------------

# run_case smoke-fresh "fresh single-file copy" ORDERED C 0 -- SRC/a.txt DST/fresh.txt

# --- summary ---------------------------------------------------------

echo "golden: $cases cases, $passed passed, $failed failed, $skipped skipped"
if [ "$failed" -gt 0 ]; then
    exit 1
fi
if [ "$PARITY_ACTIVE" != 1 ] && [ "$cases" -gt 0 ]; then
    exit 77
fi
exit 0
