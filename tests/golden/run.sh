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

# Registered containment roots. CHOPIN_TEST_FSROOT (the sprint 04D
# loopback mounts) adds secondary roots; sprint 05's EXDEV cases and
# sprint 07's reflink goldens run inside them.
containment_roots="$workreal"
for fsroot in ${CHOPIN_TEST_FSROOT:-}; do
    [ -d "$fsroot" ] && [ -w "$fsroot" ] || continue
    fsr=$(CDPATH= cd -- "$fsroot" && pwd -P) || continue
    containment_roots="$containment_roots $fsr"
done

cleanup() {
    # Failure artifacts must SURVIVE for reproduction (the harness
    # promises "artifacts kept"); only a green run cleans up.
    if [ "${failed:-0}" -gt 0 ]; then
        echo "golden: artifacts preserved under $work" >&2
        return 0
    fi
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
# Parity needs the 9.11 pin AND the feature-pinned build: a distro or
# brew 9.11 carries an unknown configure set and platform-divergent
# semantics (APFS, Darwin xattr API). CHOPIN_ORACLE_STRICT=1 overrides
# for a hand-supplied pinned oracle.
case "$oracle_ver" in
*" 9.11")
    if [ "$oracle" = "$root/build/gnu-cp/src/cp" ] \
        || [ "${CHOPIN_ORACLE_STRICT:-}" = 1 ]; then
        PARITY_ACTIVE=1
    else
        PARITY_ACTIVE=0
        echo "golden: oracle is 9.11 but not the feature-pinned build;" >&2
        echo "  self-tests run, parity cases skip ($oracle)" >&2
    fi
    ;;
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
            CHOPIN_DEBUG_VERIFY=1 \
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
    # CASE_NO_MANIFEST=1: diagnostics-only cases. A mid-abort
    # into-self copy leaves an ORDER-DEPENDENT partial tree - GNU's
    # own result varies with inode order across filesystems, so only
    # the diagnostics and rc are pinnable (overview s2 corollary).
    if [ "${CASE_NO_MANIFEST:-}" != 1 ] \
        && ! cmp -s "$cdir/A.man" "$cdir/B.man"; then
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

# --- sprint 04: metadata engine ---------------------------------------

seed_meta() {
    s="$1"
    umask 022
    mkdir -p "$s/d"
    printf 'alpha\n' > "$s/s"
    chmod 750 "$s/s"
    setfattr -n user.k1 -v v1 "$s/s" 2>/dev/null || true
    setfattr -n user.k2 -v v2 "$s/s" 2>/dev/null || true
    printf 'old\n' > "$s/e"
    chmod 664 "$s/e"
    printf 'keep\n' > "$s/ao"
    chmod 600 "$s/ao"
    touch -d '2021-03-04 05:06:07.123456789' "$s/s"
    touch -d '2020-01-01 00:00:00' "$s/e" "$s/ao"
}

meta() {
    m_flags="$1"; shift
    CASE_SEED=seed_meta
    MANIFEST_FLAGS="$m_flags"
    run_case "$@"
    CASE_SEED=
    MANIFEST_FLAGS=
}
meta "-t" meta-p-fresh "-p fresh copy" ORDERED C 0 -- -p s d/p1
meta "-t" meta-p-over "-p over existing" ORDERED C 0 -- -p s e
meta "-t -x" meta-a-file "-a on a file" ORDERED C 0 -- -a s d/p2
meta "-t" meta-times "preserve=timestamps only" ORDERED C 0 \
    -- --preserve=timestamps s d/p3
meta "" meta-mode "preserve=mode only" ORDERED C 0 \
    -- --preserve=mode s d/p4
meta "-x" meta-xattr "preserve=xattr" ORDERED C 0 \
    -- --preserve=xattr s d/p5
meta "" meta-nomode "no-preserve=mode fresh" ORDERED C 0 \
    -- --no-preserve=mode s d/p6
meta "" meta-umask "bare cp umask mode" ORDERED C 0 -- s d/p7
meta "-t" meta-attronly "--attributes-only keeps data" ORDERED C 0 \
    -- --attributes-only -p s ao
meta "-t" meta-own "preserve ownership same-uid" ORDERED C 0 \
    -- --preserve=ownership,timestamps s d/p8
meta "-t -x" meta-a-over "-a xattrs onto existing" ORDERED C 0 -- -a s e
meta "-t -x" meta-all-word "preserve=all word" ORDERED C 0 \
    -- --preserve=all s d/p9
meta "" meta-noall "no-preserve=all" ORDERED C 0 \
    -- --no-preserve=all s d/p10

# --- sprint 05: symlink/hardlink matrix -------------------------------

seed_links() {
    s="$1"
    umask 022
    mkdir -p "$s/d" "$s/dst"
    printf 'alpha\n' > "$s/f"
    ln "$s/f" "$s/g"
    ln -s f "$s/s1"
    ln -s f "$s/s2"
    ln -s nowhere "$s/dang"
    printf 'old\n' > "$s/dst/e"
    ln -s ../f "$s/d/insub"
    printf 'e2\n' > "$s/e2"
    touch -d '2021-01-01 00:00:00' "$s/e2"
    touch -d '2021-01-01 00:00:00' "$s/f" "$s/dst/e"
    touch -h -d '2021-01-01 00:00:00' "$s/s1" "$s/s2" "$s/dang" \
        "$s/d/insub" 2>/dev/null || true
}
lk() {
    lk_flags="$1"; shift
    CASE_SEED=seed_links
    MANIFEST_FLAGS="$lk_flags"
    run_case "$@"
    CASE_SEED=
    MANIFEST_FLAGS=
}
lk "" lk-P-copy "-P copies the symlink" ORDERED C 0 -- -P s1 dst/s1c
lk "" lk-P-dangling "-P copies a dangling symlink" ORDERED C 0 \
    -- -P dang dst/dc
lk "" lk-deref "default deref follows" ORDERED C 0 -- s1 dst/deref
lk "" lk-dangling-err "default dangling cannot stat" ORDERED C 1 \
    -- dang dst/nope
lk "" lk-s-relative-refused "-s relative source outside cwd" ORDERED C 1 \
    -- -s f dst/sl
lk "" lk-s-cwd "-s in current directory" ORDERED C 0 -- -s f slhere
# Relative -s demands the dest in cwd REGARDLESS of -f (quirk 4).
lk "" lk-s-force-outside "-s -f still cwd-bound" ORDERED C 1 \
    -- -s -f f dst/e
lk "" lk-s-force-cwd "-s -f replaces in cwd" ORDERED C 0 \
    -- -s -f f e2
lk "" lk-s-noforce "-s existing dest refused" ORDERED C 1 -- -s f dst/e
lk "" lk-l "-l hardlink" ORDERED C 0 -- -l f dst/l1
lk "" lk-l-existing "-l existing dest refused" ORDERED C 1 -- -l f dst/e
lk "" lk-l-force "-l -f atomic replace" ORDERED C 0 -- -l -f f dst/e
lk "" lk-l-force-v "removed line on -l -f -v" ORDERED C 0 \
    -- -l -f -v f dst/e
lk "" lk-l-P-symlink "-l -P hardlinks the symlink itself" ORDERED C 0 \
    -- -l -P s1 dst/lp
lk "" lk-d "-d preserves the symlink" ORDERED C 0 -- -d s1 dst/ds
lk "-t" lk-P-p-times "-P -p preserves symlink times" ORDERED C 0 \
    -- -P -p s1 dst/spt
lk "" lk-preserve-links "hardlink pair collapses" ORDERED C 0 \
    -- -L --preserve=links f g dst
lk "" lk-links-symfile "file+symlink collapse under -L" ORDERED C 0 \
    -- -L --preserve=links f s1 dst
lk "" lk-H "-H top-level deref" ORDERED C 0 -- -H s1 dst/h1
lk "" lk-multilink-plain "hardlink pair copies twice plain" ORDERED C 0 \
    -- f g dst
lk "" lk-update-older-samelink "DEV-003 kept quirk" ORDERED C 0 \
    -- -P --update=older s1 s2

# EXDEV: only meaningful where a second filesystem root is registered
# (the 04D fs job); the first registered root hosts the cross-device
# destination.
for exroot in ${CHOPIN_TEST_FSROOT:-}; do
    [ -d "$exroot" ] && [ -w "$exroot" ] || continue
    cases=$((cases + 1))
    if [ "$PARITY_ACTIVE" != 1 ]; then
        skipped=$((skipped + 1))
        break
    fi
    exdst="$exroot/chopin-exdev.$$"
    rmtree "$exdst"; mkdir -p "$exdst/A" "$exdst/B"
    mkpair case-exdev seed_links
    cdir="$work/case-exdev"
    for f in A.out A.err B.out B.err A.rc B.rc A.man B.man; do
        : > "$cdir/$f"
    done
    for side in A B; do
        if [ "$side" = A ]; then tool="$oracle"; else tool="$uut"; fi
        run_pinned C "$cdir/$side" "$tool" -l f "$exdst/$side/l" \
            > "$cdir/$side.out" 2> "$cdir/$side.err" < /dev/null
        echo $? > "$cdir/$side.rc"
        assert_contained "$exdst/$side"
    done
    # -l across devices: EXDEV failure bytes must match. The dest
    # paths differ per side (A/ vs B/); normalize them like the
    # program token.
    normprog < "$cdir/A.err" \
        | sed "s|$exdst/A|EXROOT|g" > "$cdir/A.err.n"
    normprog < "$cdir/B.err" \
        | sed "s|$exdst/B|EXROOT|g" > "$cdir/B.err.n"
    if [ "$(cat "$cdir/A.rc")" = "$(cat "$cdir/B.rc")" ] \
        && cmp -s "$cdir/A.err.n" "$cdir/B.err.n"; then
        passed=$((passed + 1))
        rmtree "$cdir"
    else
        fail_case exdev "-l across devices differs"
        diff "$cdir/A.err.n" "$cdir/B.err.n" | head -4
    fi
    rmtree "$exdst"
    break
done

# --- sprint 06: recursion ---------------------------------------------

seed_tree() {
    s="$1"
    umask 022
    mkdir -p "$s/t/sub/deep" "$s/t/empty" "$s/dst" "$s/have/t"
    printf 'a\n' > "$s/t/a"
    printf 'b\n' > "$s/t/sub/b"
    printf 'c\n' > "$s/t/sub/deep/c"
    chmod 2750 "$s/t/sub"
    ln -s ../a "$s/t/sub/lnk"
    ln "$s/t/a" "$s/t/ahard"
    mkfifo "$s/t/fifo"
    ln -s loop "$s/t/loopy" 2>/dev/null || true
    printf 'x\n' > "$s/have/t/old"
    touch -d '2021-01-01 00:00:00' "$s/t/a" "$s/t/sub/b" \
        "$s/t/sub/deep/c" "$s/have/t/old"
    touch -h -d '2021-01-01 00:00:00' "$s/t/sub/lnk" "$s/t/loopy" \
        2>/dev/null || true
    touch -d '2021-01-01 00:00:00' "$s/t/sub/deep" "$s/t/sub" \
        "$s/t/empty" "$s/t"
}
tr_() {
    tr_flags="$1"; shift
    CASE_SEED=seed_tree
    MANIFEST_FLAGS="$tr_flags"
    run_case "$@"
    CASE_SEED=
    MANIFEST_FLAGS=
}
tr_ "" rec-fresh "-R fresh tree" ORDERED C 0 -- -R t dst/t1
tr_ "" rec-into-dir "-R into existing dir" ORDERED C 0 -- -R t dst
tr_ "" rec-merge "-R merge over partial dest" ORDERED C 0 -- -R t have
tr_ "-t" rec-archive "-a full tree" SORTED C 0 -- -a t dst/t2
tr_ "" rec-verbose "-Rv sorted streams" SORTED C 0 -- -Rv t dst/t3
tr_ "" rec-onto-file "-R dir onto non-dir" ORDERED C 1 -- -R t t/a
tr_ "" rec-lowercase "-r equals -R" ORDERED C 0 -- -r t dst/t4
tr_ "-t" rec-p-dirtimes "-R -p dir times post-order" SORTED C 0 \
    -- -R -p t dst/t5
tr_ "" rec-specials "fifo travels under -R" ORDERED C 0 -- -R t dst/t6
tr_ "" rec-x "-x on one filesystem" ORDERED C 0 -- -R -x t dst/t7
tr_ "" rec-links "-R --preserve=links" ORDERED C 0 \
    -- -R --preserve=links t dst/t8
# -RL derefs everything: the dangling loopy makes BOTH tools fail.
tr_ "" rec-L "-RL derefs, dangling member fails" SORTED C 1 \
    -- -RL t dst/t9
tr_ "" rec-H "-RH top-level only" SORTED C 0 -- -RH t dst/t10
tr_ "" rec-src-twice "source dir twice warns" SORTED C 0 -- -R t t dst
tr_ "" rec-no-R "dir without -R omitted" ORDERED C 1 -- t dst/nope
# Into-self: diagnostics + rc only (order-dependent partial trees).
CASE_NO_MANIFEST=1
tr_ "" rec-into-self "cp -R dir dir" ORDERED C 1 -- -R t t
tr_ "" rec-into-sub "cp -R dir dir/sub" ORDERED C 1 -- -R t t/sub
tr_ "" rec-dotdot "cp -R d/.. quirk" ORDERED C 1 -- -R t/.. dst2
CASE_NO_MANIFEST=

# --parents matrix.
tr_ "" par-file "--parents single file" ORDERED C 0 \
    -- --parents t/sub/deep/c dst
tr_ "" par-verbose "--parents -v per-dir lines" ORDERED C 0 \
    -- --parents -v t/sub/deep/c dst
tr_ "-t" par-reprotect "--parents -p re_protect" ORDERED C 0 \
    -- --parents -p t/sub/deep/c dst
tr_ "" par-partial "--parents partially existing" ORDERED C 0 \
    -- --parents t/a dst
tr_ "" par-nomode "--parents --no-preserve=mode" ORDERED C 0 \
    -- --parents --no-preserve=mode t/sub/deep/c dst
tr_ "" par-subtree "--parents -R subtree" SORTED C 0 \
    -- --parents -R t/sub dst

# --- sprint 07: data engines ------------------------------------------

seed_engine() {
    s="$1"
    umask 022
    mkdir -p "$s/dst"
    printf 'plain\n' > "$s/plain"
    : > "$s/sparse"
    dd if=/dev/zero of="$s/sparse" bs=1 count=8 seek=1048576 \
        conv=notrunc 2>/dev/null
    printf 'DATA' | dd of="$s/sparse" bs=1 seek=2097152 \
        conv=notrunc 2>/dev/null
    truncate -s 4194304 "$s/sparse"
    dd if=/dev/zero of="$s/zeros" bs=4096 count=64 2>/dev/null
    seq 1 20000 > "$s/data"
    touch -d '2021-01-01 00:00:00' "$s/plain" "$s/sparse" "$s/zeros" \
        "$s/data"
}
en() {
    CASE_SEED=seed_engine
    run_case "$@"
    CASE_SEED=
}
en eng-plain "plain through the ladder" ORDERED C 0 -- plain dst/p
en eng-sparse-auto "sparse preserved under auto" ORDERED C 0 \
    -- sparse dst/s
en eng-sparse-always "zeros punched under always" ORDERED C 0 \
    -- --sparse=always zeros dst/z
en eng-sparse-never "holes expanded under never" ORDERED C 0 \
    -- --sparse=never sparse dst/sn
en eng-offload "offload lane data" ORDERED C 0 -- data dst/r
en eng-reflink-never "reflink=never pure read/write" ORDERED C 0 \
    -- --reflink=never data dst/rn
en eng-reflink-always "reflink=always fatal off-CoW" ORDERED C - \
    -- --reflink=always data dst/ra
en eng-debug-plain "--debug vocabulary plain" ORDERED C 0 \
    -- --debug plain dst/dp
en eng-debug-sparse "--debug SEEK_HOLE" ORDERED C 0 \
    -- --debug sparse dst/ds
en eng-debug-zeros "--debug zeros under always" ORDERED C 0 \
    -- --debug --sparse=always zeros dst/dz
en eng-debug-avoided "--debug avoided under reflink=never" ORDERED C 0 \
    -- --debug --reflink=never data dst/dr
en eng-debug-sn "--debug sparse=never still scans" ORDERED C 0 \
    -- --debug --sparse=never sparse dst/dsn
# Note eng-reflink-always uses rc '-': on btrfs/XFS fs-lanes the clone
# SUCCEEDS (rc 0), on ext4/tmpfs it is fatal (rc 1) - agreement is the
# assertion; the manifest still must match.

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
