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

# Permission-trap-safe removal (555-dir seeds).
rmtree() {
    chmod -R u+rwx "$1" 2>/dev/null || true
    rm -rf "$1"
}

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

# Normalize the program token of diagnostic lines so oracle and UUT
# stderr compare. Three classes (measured against the pinned oracle):
# error()-class lines prefix the BASENAME ("cp: ...", "chopin: ...");
# getopt-class lines prefix argv[0] VERBATIM (an absolute path under
# run_pinned); the Try-help line embeds verbatim argv[0] in quotes.
normprog() {
    sed \
        -e 's|^[^:]*/*cp:|PROG:|' \
        -e 's|^[^:]*/*gcp:|PROG:|' \
        -e 's|^[^:]*/*chopin:|PROG:|' \
        -e 's|^[^:]*/*cpn:|PROG:|' \
        -e "s|^Try '[^']* --help' for more information\.|Try 'PROG --help' for more information.|"
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
    rmtree "$work/$mp_case"
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
    failed_before=$failed

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
        printf '%s' "${CASE_STDIN:-}" \
            | run_pinned "$lc" "$sb" "$tool" "$@" \
            > "$cdir/$side.out" 2> "$cdir/$side.err"
        echo $? > "$cdir/$side.rc"
        assert_contained "$sb"
        # Manifest the WHOLE sandbox: destination results and the
        # proof that sources were not touched.
        "$manifest" ${MANIFEST_FLAGS:-} "$sb" > "$cdir/$side.man" \
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
    if [ "$ok" = 1 ] && [ "$failed_before" = "$failed" ]; then
        passed=$((passed + 1))
        rmtree "$cdir"
    else
        echo "  artifacts kept: $cdir"
    fi
}

# --- deviation tier --------------------------------------------------
# run_case_dev TAG desc lc -- <args>: the UUT must byte-match the
# PINNED corrected behavior (tests/golden/dev/TAG.{out,err,rc,man},
# generated once and reviewed) while the ORACLE must still differ
# from the pin - an upstream fix trips DEVIATION VANISHED and forces
# re-evaluating doc/deviations.md. Cases arrive with their fixes
# (sprint 03 onward).
run_case_dev() {
    dv_tag="$1"; dv_desc="$2"; dv_lc="$3"
    shift 3
    [ "$1" = "--" ] || { echo "run_case_dev $dv_tag: missing --" >&2; exit 1; }
    shift
    cases=$((cases + 1))
    pin="$root/tests/golden/dev/$dv_tag"
    if [ ! -f "$pin.err" ]; then
        echo "run_case_dev $dv_tag: pin files missing" >&2
        failed=$((failed + 1))
        return 0
    fi
    mkpair "dev-$dv_tag" "${CASE_SEED:-seed_smoke}"
    cdir="$work/dev-$dv_tag"
    for f in A.out A.err B.out B.err A.rc B.rc; do : > "$cdir/$f"; done
    for side in A B; do
        if [ "$side" = A ]; then tool="$oracle"; else tool="$uut"; fi
        sb="$cdir/$side"
        printf '%s' "${CASE_STDIN:-}" \
            | run_pinned "$dv_lc" "$sb" "$tool" "$@" \
            > "$cdir/$side.out" 2> "$cdir/$side.err"
        echo $? > "$cdir/$side.rc"
        assert_contained "$sb"
    done
    ok=1
    normprog < "$cdir/B.err" > "$cdir/B.err.n"
    if ! cmp -s "$cdir/B.err.n" "$pin.err" \
        || [ "$(cat "$cdir/B.rc")" != "$(cat "$pin.rc")" ]; then
        fail_case "dev-$dv_tag" "UUT does not match the pinned fix"
        diff "$pin.err" "$cdir/B.err.n" | head -6
        ok=0
    fi
    normprog < "$cdir/A.err" > "$cdir/A.err.n"
    if cmp -s "$cdir/A.err.n" "$pin.err" \
        && [ "$(cat "$cdir/A.rc")" = "$(cat "$pin.rc")" ]; then
        fail_case "dev-$dv_tag" \
            "DEVIATION VANISHED: oracle now matches the pin; re-evaluate doc/deviations.md"
        ok=0
    fi
    if [ "$ok" = 1 ]; then
        passed=$((passed + 1))
        rmtree "$cdir"
    fi
}

# Filter-file integrity: no DEVIATIONS line without a register entry.
while read -r dev_id _; do
    case "$dev_id" in ''|'#'*) continue ;; esac
    grep -q "^## $dev_id " "$root/doc/deviations.md" || {
        echo "golden: DEVIATIONS filter names $dev_id absent from doc/deviations.md" >&2
        exit 1
    }
done < "$root/tests/golden/DEVIATIONS"

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
    rmtree "$cdir"
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

# --- case matrix -----------------------------------------------------
# Sprint 01: the error-wording tier. Operand-shape, conflict, and
# argmatch errors byte-match the oracle (normprog'd). Copy cases
# arrive with sprint 02.

run_case err-missing-all "no operands" ORDERED C 1 --
run_case err-missing-dst "one operand" ORDERED C 1 -- onlyone
run_case err-hard-sym "-l with -s" ORDERED C 1 -- -l -s SRC/a.txt DST/x
run_case err-backup-n "-b with -n" ORDERED C 1 -- -b -n SRC/a.txt DST/x
run_case err-n-backup "-n then -b" ORDERED C 1 -- -n -b SRC/a.txt DST/x
run_case err-backup-nonefail "--backup with --update=none-fail" ORDERED C 1 \
    -- --backup --update=none-fail SRC/a.txt DST/x
run_case err-reflink-sparse "--reflink=always --sparse=never" ORDERED C 1 \
    -- --reflink=always --sparse=never SRC/a.txt DST/x
run_case err-reflink-sparse-always "--reflink --sparse=always" ORDERED C 1 \
    -- --reflink --sparse=always SRC/a.txt DST/x
run_case err-sparse-amb "--sparse=a ambiguous" ORDERED C 1 \
    -- --sparse=a SRC/a.txt DST/x
run_case err-sparse-amb-utf8 "--sparse=a ambiguous, UTF-8 quotes" \
    ORDERED en_US.UTF-8 1 -- --sparse=a SRC/a.txt DST/x
run_case err-sparse-inv "--sparse=bogus invalid" ORDERED C 1 \
    -- --sparse=bogus SRC/a.txt DST/x
run_case err-update-amb "--update=n ambiguous" ORDERED C 1 \
    -- --update=n SRC/a.txt DST/x
run_case err-update-non "--update=non ambiguous" ORDERED C 1 \
    -- --update=non SRC/a.txt DST/x
run_case err-backup-word "--backup=n ambiguous" ORDERED C 1 \
    -- --backup=n SRC/a.txt DST/x
run_case err-preserve-empty "--preserve trailing comma" ORDERED C 1 \
    -- --preserve=mode, SRC/a.txt DST/x
run_case err-preserve-inv "--preserve=bogus" ORDERED C 1 \
    -- --preserve=bogus SRC/a.txt DST/x
run_case err-nopreserve-list "--no-preserve list invalid tail" ORDERED C 1 \
    -- --no-preserve=mode,bogus SRC/a.txt DST/x
run_case err-preserve-ctx "--preserve=context non-selinux" ORDERED C 1 \
    -- --preserve=context SRC/a.txt DST/x
run_case err-long-amb "--p ambiguous" ORDERED C 1 -- --p SRC/a.txt DST/x
run_case err-long-amb-no "--no ambiguous" ORDERED C 1 -- --no SRC/a.txt DST/x
run_case err-long-unrec "unrecognized long" ORDERED C 1 \
    -- --bogus SRC/a.txt DST/x
run_case err-short-inv "invalid short" ORDERED C 1 -- -q SRC/a.txt DST/x
run_case err-long-noarg "--link takes no argument" ORDERED C 1 \
    -- --link=x SRC/a.txt DST/x
run_case err-long-reqarg "--suffix requires argument" ORDERED C 1 -- --suffix
run_case err-sparse-noarg "--sparse requires argument at argv end" \
    ORDERED C 1 -- SRC/a.txt DST/x --sparse
run_case err-t-T "-t with -T" ORDERED C 1 -- -t DST -T SRC/a.txt DST/x
run_case err-extra-T "-T extra operand" ORDERED C 1 \
    -- -T SRC/a.txt DST/x DST/y
run_case err-multi-t "duplicate -t" ORDERED C 1 -- -t DST -t DST SRC/a.txt
run_case err-t-missing "-t nonexistent dir" ORDERED C 1 \
    -- -t no-such-dir SRC/a.txt
run_case err-t-notdir "-t non-directory" ORDERED C 1 \
    -- -t SRC/a.txt SRC/sub/b.txt
run_case err-3op-notdir "three operands last not dir" ORDERED C 1 \
    -- SRC/a.txt SRC/sub/b.txt no-such-target
run_case err-parents-nondir "--parents non-dir dest" ORDERED C 1 \
    -- --parents SRC/a.txt no-such-dest

# --- sprint 02: single-file copy matrix ------------------------------
# Twin-sandbox manifest comparison; --reflink=always and --debug
# engine-line comparisons stay OUT until sprint 07 (locked interim
# divergence).

seed_copy() {
    s="$1"
    umask 022
    mkdir -p "$s/src" "$s/dst/adir"
    printf 'alpha\n' > "$s/src/a.txt"
    printf 'beta\n' > "$s/src/b.txt"
    chmod 640 "$s/src/b.txt"
    printf 'old\n' > "$s/dst/a.txt"
    printf 'oldc\n' > "$s/dst/old.txt"
    touch -d '2020-01-01 00:00:00' "$s/dst/old.txt"
    ln -s dangle-target "$s/dst/dangling"
    printf 'x\n' > "$s/src/unread.txt"
    chmod 000 "$s/src/unread.txt"
    mkdir "$s/dst/nowrite"
    printf 'w\n' > "$s/dst/nowrite/keep.txt"
    chmod 555 "$s/dst/nowrite"
}

CASE_SEED=seed_copy
run_case cp-fresh "fresh single-file copy" ORDERED C 0 \
    -- SRC/a.txt DST/fresh.txt
run_case cp-overwrite "overwrite existing dst" ORDERED C 0 \
    -- SRC/a.txt DST/a.txt
run_case cp-into-dir "multi-source into dir" ORDERED C 0 \
    -- SRC/a.txt SRC/b.txt DST
run_case cp-mode "source mode travels to new dst" ORDERED C 0 \
    -- SRC/b.txt DST/mode.txt
run_case cp-v "verbose bytes" ORDERED C 0 -- -v SRC/a.txt DST/v.txt
run_case cp-v-multi "verbose multi-source" ORDERED C 0 \
    -- -v SRC/a.txt SRC/b.txt DST
run_case cp-n "-n silent skip exit 0" ORDERED C 0 \
    -- -n SRC/a.txt DST/a.txt
CASE_STDIN='y
'
run_case cp-i-yes "-i answered yes" ORDERED C 0 -- -i SRC/a.txt DST/a.txt
CASE_STDIN='n
'
run_case cp-i-no "-i declined exit 1 silent" ORDERED C 1 \
    -- -i SRC/a.txt DST/a.txt
CASE_STDIN=
run_case cp-i-eof "-i EOF declines" ORDERED C 1 -- -i SRC/a.txt DST/a.txt
CASE_STDIN='y
'
run_case cp-i-unwritable "unwritable prompt wording" ORDERED C 0 \
    -- -i SRC/a.txt DST/old.txt
CASE_STDIN=
run_case cp-f "-f over existing" ORDERED C 0 -- -f SRC/a.txt DST/a.txt
run_case cp-update-older-skip "--update=older newer dst skips" ORDERED C 0 \
    -- --update=older SRC/a.txt DST/old.txt
run_case cp-update-none "--update=none silent skip" ORDERED C 0 \
    -- --update=none SRC/a.txt DST/a.txt
run_case cp-update-nonefail "--update=none-fail refuses" ORDERED C 1 \
    -- --update=none-fail SRC/a.txt DST/a.txt
run_case cp-unreadable "unreadable source" ORDERED C 1 \
    -- SRC/unread.txt DST/u.txt
run_case cp-unwritable-dir "unwritable dst dir" ORDERED C 1 \
    -- SRC/a.txt DST/nowrite/new.txt
run_case cp-same-file "same file refused" ORDERED C 1 \
    -- DST/a.txt DST/a.txt
run_case cp-T-ontodir "-T onto existing dir" ORDERED C 1 \
    -- -T SRC/a.txt DST/adir
run_case cp-remove-dest "--remove-destination -v" ORDERED C 0 \
    -- -v --remove-destination SRC/a.txt DST/a.txt
run_case cp-trailing-slash "EISDIR->ENOTDIR trailing slash" ORDERED C 1 \
    -- SRC/a.txt DST/nonexist/
run_case cp-dangling-dest "dangling dest symlink refused" ORDERED C 1 \
    -- SRC/a.txt DST/dangling
run_case cp-context-warn "--context=ctx warns then copies" ORDERED C 0 \
    -- --context=ctx SRC/a.txt DST/ctx.txt
run_case cp-debug-skip "-n --debug prints skipped" ORDERED C 0 \
    -- -n --debug SRC/a.txt DST/a.txt
CASE_SEED=

# --- sprint 03: same-file matrix, backups, dev tier ------------------

seed_backup() {
    s="$1"
    umask 022
    mkdir -p "$s/d"
    printf 'alpha\n' > "$s/d/a"
    printf 'old\n' > "$s/d/b"
    touch -d '2020-01-01 00:00:00' "$s/d/b"
    ln "$s/d/b" "$s/d/bhard"
    printf 'v0\n' > "$s/d/n"
    printf 'v1\n' > "$s/d/n.~1~"
    printf 'w9\n' > "$s/d/n9"
    printf 'x9\n' > "$s/d/n9.~9~"
    touch -d '2020-01-01 00:00:00' "$s/d/n" "$s/d/n.~1~" \
        "$s/d/n9" "$s/d/n9.~9~"
    : > "$s/a"
    printf 'A\n' > "$s/a~"
    printf 'u\n' > "$s/d/unread"
    chmod 000 "$s/d/unread"
}

# Backup cases address the seed's d/ tree with literal relative
# paths (cwd = sandbox root). VAR=val before a FUNCTION call has
# unspecified persistence in POSIX sh (family trap) - set/reset
# explicitly.
bk() {
    CASE_SEED=seed_backup
    run_case "$@"
    CASE_SEED=
}
bk bk-simple "simple backup" ORDERED C 0 -- -b d/a d/b
bk bk-simple2 "simple backup word" ORDERED C 0 -- --backup=simple d/a d/b
bk bk-numbered "numbered first backup" ORDERED C 0 \
    -- --backup=numbered d/a d/b
bk bk-numbered-next "numbered continues from max" ORDERED C 0 \
    -- --backup=numbered d/a d/n
bk bk-existing-numbered "existing picks numbered" ORDERED C 0 \
    -- -b d/a d/n
bk bk-existing-simple "existing picks simple" ORDERED C 0 -- -b d/a d/b
bk bk-all9s "all-9s numbered growth" ORDERED C 0 \
    -- --backup=numbered d/a d/n9
bk bk-suffix "explicit -S suffix" ORDERED C 0 -- -b -S .bak d/a d/b
bk bk-same-same "cp -b f f refused" ORDERED C 1 -- -b d/a d/a
bk bk-hardlink "cp -b f hardlink-of-f backs up" ORDERED C 0 \
    -- -b d/b d/bhard
bk bk-l-noop "cp -l f f silent no-op" ORDERED C 0 -- -l d/a d/a
bk bk-fb-rewrite "cp -f -b same same dest rewrite" ORDERED C 0 \
    -- -f -b d/a d/a
bk bk-unbackup "failed copy restores the backup" ORDERED C 1 \
    -- -b d/unread d/b
bk bk-unbackup-v "unbackup verbose line" ORDERED C 1 \
    -- -bv d/unread d/b
bk bk-verbose "backup annotation in -v" ORDERED C 0 -- -bv d/a d/b
bk bk-update-older "backup with update-older copies older dst" \
    ORDERED C 0 -- -b --update=older d/a d/b

# >=2-source guard tables (2.7).
seed_multi() {
    s="$1"
    umask 022
    mkdir -p "$s/d1" "$s/d2" "$s/dst"
    printf 'one\n' > "$s/d1/f"
    printf 'two\n' > "$s/d2/f"
    printf 'a\n' > "$s/a"
}
mg() {
    CASE_SEED=seed_multi
    run_case "$@"
    CASE_SEED=
}
mg guard-dup-source "duplicate source warns, succeeds" ORDERED C 0 \
    -- a ./a dst
mg guard-clobber "will not overwrite just-created" ORDERED C 1 \
    -- d1/f d2/f dst
mg guard-clobber-numbered "numbered backups bypass the guard" ORDERED C 0 \
    -- --backup=numbered d1/f d2/f dst
mg guard-dup-backup-off "backups disable the dup-source skip, guard fires" ORDERED C 1 \
    -- -b a ./a dst

# Dev tier: DEV-001 single-space fix, pinned corrected bytes.
CASE_SEED=seed_backup
run_case_dev backup-space "DEV-001 single-space refusal" C \
    -- --b=simple 'a~' a
CASE_SEED=

# The unwritable-dir seed leaves a 555 directory; the cleanup trap's
# chmod -R handles it.

# stdout write-error path (atexit close_stdout, gnu-cp-analysis.md
# 4.1): needs /dev/full - Linux lanes only, clean skip elsewhere.
if [ -w /dev/full ] && [ "$PARITY_ACTIVE" = 1 ]; then
    cases=$((cases + 1))
    mkpair case-devfull seed_copy
    cdir="$work/case-devfull"
    for f in A.err B.err A.rc B.rc; do : > "$cdir/$f"; done
    for side in A B; do
        if [ "$side" = A ]; then tool="$oracle"; else tool="$uut"; fi
        sb="$cdir/$side"
        run_pinned C "$sb" "$tool" -v src/a.txt dst/full.txt \
            > /dev/full 2> "$cdir/$side.err" < /dev/null
        echo $? > "$cdir/$side.rc"
    done
    normprog < "$cdir/A.err" > "$cdir/A.err.n"
    normprog < "$cdir/B.err" > "$cdir/B.err.n"
    if [ "$(cat "$cdir/A.rc")" = "$(cat "$cdir/B.rc")" ] \
        && cmp -s "$cdir/A.err.n" "$cdir/B.err.n"; then
        passed=$((passed + 1))
        rmtree "$cdir"
    else
        fail_case devfull "write-error path differs"
        diff "$cdir/A.err.n" "$cdir/B.err.n" | head -6
    fi
fi

# --- summary ---------------------------------------------------------

echo "golden: $cases cases, $passed passed, $failed failed, $skipped skipped"
if [ "$failed" -gt 0 ]; then
    exit 1
fi
if [ "$PARITY_ACTIVE" != 1 ] && [ "$cases" -gt 0 ]; then
    exit 77
fi
exit 0
