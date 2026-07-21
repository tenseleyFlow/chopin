#!/bin/sh
# Differential fuzzer (sprint 08): random source trees x random
# pre-existing DEST STATES x random flag draws, chopin vs the pinned
# oracle, comparing result-tree manifests, sorted stdout, sorted
# normalized stderr, and exit status.
#
# Deterministic per (FUZZ_SEED, trial) on a given box (awk rand()
# varies across implementations - same-box repro, rank's precedent).
#
# DESTRUCTIVE-TOOL SAFETY: sandboxes live under a mktemp workdir;
# both tools run chdir'd inside their own sandbox with relative
# operands only; capture files are pre-created OUTSIDE the compared
# trees (the ..-size lesson); failure artifacts save BOTH sandboxes
# plus the plan.
#
# Deviation filter: stderr diffs matching doc/deviations.md entries
# listed in tests/golden/DEVIATIONS are normalized away (currently
# DEV-001: GNU's double space after "destroy source;").
#
# Env: FUZZ_TRIALS (default 100), FUZZ_SEED (default 42).
# Exit: 0 pass, 1 fail, 77 skip (no pinned oracle / root).
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
cd "$root"

trials="${FUZZ_TRIALS:-100}"
seed="${FUZZ_SEED:-42}"
# FUZZ_IDENTITY=1 (sprint 09C): A = chopin serial, B = chopin
# parallel with a seed-derived worker count (FUZZ_CHUNKS=1 also
# forces chunked dispatch at a 4K threshold). Same binary both
# sides, so every stream and the manifest compare BYTE-EXACT - no
# sorting, no normalization, no deviation filter, no oracle needed.
identity="${FUZZ_IDENTITY:-0}"

if [ "$(id -u)" = 0 ]; then
    echo "fuzz: SKIPPED - refusing to run as root" >&2
    exit 77
fi

if [ "$identity" = 1 ]; then
    oracle="$root/chopin"
else
    oracle=$(sh scripts/find-gnu-cp.sh) || {
        echo "fuzz: no oracle; skipping (77)" >&2
        exit 77
    }
    case $("$oracle" --version | sed -n 1p) in
    *" 9.11") ;;
    *) echo "fuzz: oracle not the 9.11 pin; skipping (77)" >&2; exit 77 ;;
    esac
    if [ "$oracle" != "$root/build/gnu-cp/src/cp" ] \
        && [ "${CHOPIN_ORACLE_STRICT:-}" != 1 ]; then
        echo "fuzz: oracle not the feature-pinned build; skipping (77)" >&2
        exit 77
    fi
fi

manifest="$root/build/manifest"
[ -x "$manifest" ] || {
    echo "fuzz: manifest tool missing; run make check first" >&2
    exit 1
}

work=$(mktemp -d "${TMPDIR:-/tmp}/chopin-fuzz.XXXXXX") || exit 1
trap 'chmod -R u+rwx "$work" 2>/dev/null; rm -rf "$work"' EXIT INT TERM
faildir="$root/tests/.work/fuzz-failures"
# Stale artifacts of THIS seed mislead debugging: scrub them, but
# leave other seeds' evidence alone (a later smoke run must not
# destroy a campaign's findings).
chmod -R u+rwx "$faildir" 2>/dev/null || true
rm -rf "$faildir/trial-$seed-"*

rmtree() {
    chmod -R u+rwx "$1" 2>/dev/null || true
    rm -rf "$1"
}

normprog() {
    # Prompts have no trailing newline, so consecutive prompts share
    # one line in TRAVERSAL order; split at "? " so the sorted compare
    # sees an order-free prompt SET, then normalize program tokens.
    awk '{ gsub(/\? /, "?\n"); print }' \
    | sed -e '/^$/d' \
          -e 's|^[^:]*/*cp:|PROG:|' -e 's|^[^:]*/*chopin:|PROG:|' \
          -e "s|^Try '[^']* --help' for more information\.|TRYHELP|"
}

# Active deviation normalizations, keyed on tests/golden/DEVIATIONS.
apply_deviation_filter() {
    df_sed=""
    if grep -q '^DEV-001' "$root/tests/golden/DEVIATIONS" 2>/dev/null; then
        df_sed="-e s/destroy source;  /destroy source; /"
    fi
    if [ -n "$df_sed" ]; then
        # shellcheck disable=SC2086
        sed $df_sed
    else
        cat
    fi
}

# Emit one trial plan. Lines:
#   F <path> <content-class> <mode>     regular file
#   D <path> <mode>                     directory
#   L <path> <target>                   symlink
#   H <path> <existing-path>            hardlink
#   S <path> <kib>                      sparse file
#   P <path>                            fifo
#   ROOT src|dst                        section marker
#   ARGS <tokens...>                    invocation (relative operands)
#   STDIN <y|n|none>
gen_plan() {
    awk -v seed="$seed" -v trial="$1" '
    # Name classes: plain, spaces, utf8, quotes, tabs - cp quotes
    # operands uniformly, so every class is fair game.
    function newname(   r, base) {
        r = rand()
        if (r < 0.5) base = sprintf("f%d", int(rand() * 1000))
        else if (r < 0.7) base = sprintf("sp %d", int(rand() * 100))
        else if (r < 0.8) base = sprintf("u\342\230\203%d", int(rand() * 100))
        else if (r < 0.9) base = sprintf("q\047%d", int(rand() * 100))
        else base = sprintf("t\011%d", int(rand() * 100))
        return base
    }
    BEGIN {
        srand(seed + trial)
        US = "\037"

        print "ROOT src"
        ndirs = 1; dirs[0] = "s"
        printf "D%ss%s755\n", US, US
        nfiles = 0
        nsrc = 3 + int(rand() * 8)
        for (i = 0; i < nsrc; i++) {
            d = dirs[int(rand() * ndirs)]
            n = newname()
            path = d "/" n
            r = rand()
            if (r < 0.45) {
                printf "F%s%s%s%d%s%o\n", US, path, US, int(rand() * 5), US, \
                    (rand() < 0.2 ? 384 : 420) + (rand() < 0.1 ? 64 : 0)
                files[nfiles++] = path
            } else if (r < 0.6 && ndirs < 5) {
                printf "D%s%s%s755\n", US, path, US
                dirs[ndirs++] = path
            } else if (r < 0.75) {
                if (nfiles > 0 && rand() < 0.7)
                    printf "L%s%s%s%s\n", US, path, US, files[int(rand() * nfiles)]
                else
                    printf "L%s%s%snowhere-%d\n", US, path, US, int(rand() * 10)
            } else if (r < 0.85 && nfiles > 0) {
                printf "H%s%s%s%s\n", US, path, US, files[int(rand() * nfiles)]
                files[nfiles++] = path
            } else if (r < 0.95) {
                printf "S%s%s%s%d\n", US, path, US, 64 + int(rand() * 512)
                files[nfiles++] = path
            } else {
                printf "P%s%s\n", US, path
            }
        }

        print "ROOT dst"
        dr = rand()
        if (dr < 0.3) {
            # absent dest (fresh)
        } else if (dr < 0.6) {
            printf "D%sd%s755\n", US, US
        } else if (dr < 0.8) {
            # partial overlap: dest dir contains some same-named files
            printf "D%sd%s755\n", US, US
            printf "D%sd/s%s755\n", US, US
            for (i = 0; i < nfiles && i < 3; i++)
                if (rand() < 0.6)
                    # Overlap files stay writable: an unwritable dest
                    # FILE makes hardlink-group outcomes traversal-
                    # order-dependent (GNU differs across filesystems
                    # too - fuzz 555-9); dir traps keep permission
                    # coverage.
                    printf "F%sd/%s%s%d%s644\n", US, files[i], US, int(rand() * 5), US
        } else if (dr < 0.9) {
            # type conflict: dest path exists as a FILE
            printf "F%sd%s0%s420\n", US, US, US
        } else {
            printf "D%sd%s755\n", US, US
            printf "L%sd/trap%ss\n", US, US
        }

        # Flags
        nf = 0
        if (rand() < 0.75) flags[nf++] = "-R"
        r = rand()
        if (r < 0.15) flags[nf++] = "-p"
        else if (r < 0.25) flags[nf++] = "-a"
        else if (r < 0.30) flags[nf++] = "--preserve=timestamps"
        else if (r < 0.35) flags[nf++] = "--no-preserve=mode"
        r = rand()
        if (r < 0.10) flags[nf++] = "-l"
        else if (r < 0.18) flags[nf++] = "-s"
        r = rand()
        if (r < 0.15) flags[nf++] = "-P"
        else if (r < 0.25) flags[nf++] = "-L"
        else if (r < 0.30) flags[nf++] = "-H"
        r = rand()
        stdin_ans = "none"
        if (r < 0.10) flags[nf++] = "-n"
        else if (r < 0.18) { flags[nf++] = "-i"; stdin_ans = (rand() < 0.5 ? "y" : "n") }
        else if (r < 0.25) flags[nf++] = "-f"
        r = rand()
        if (r < 0.10) flags[nf++] = "-b"
        else if (r < 0.15) flags[nf++] = "--backup=numbered"
        r = rand()
        if (r < 0.10) flags[nf++] = "--sparse=always"
        else if (r < 0.15) flags[nf++] = "--sparse=never"
        r = rand()
        if (r < 0.08) flags[nf++] = "--reflink=never"
        if (rand() < 0.20) flags[nf++] = "-v"
        if (rand() < 0.10) flags[nf++] = "--debug"
        if (rand() < 0.08) flags[nf++] = "-x"
        if (rand() < 0.10) flags[nf++] = "--remove-destination"
        if (rand() < 0.08) flags[nf++] = "-T"
        if (rand() < 0.05) flags[nf++] = "--strip-trailing-slashes"

        # -l/-s + -b conflicts are legal; -n + -b is a usage error both
        # tools share - keep it (parity of the error is the point).

        printf "ARGS"
        for (i = 0; i < nf; i++) printf "%s%s", US, flags[i]
        # Operands: the tree, sometimes a file source too, + dest
        printf "%ss", US
        if (rand() < 0.25 && nfiles > 0)
            printf "%s%s", US, files[int(rand() * nfiles)]
        printf "%sd\n", US
        printf "STDIN %s\n", stdin_ans
    }'
}

# Build one sandbox from the plan (src+dst sections).
build_sandbox() {
    bs_dir="$1"
    bs_plan="$2"
    mkdir -p "$bs_dir"
    (
        cd "$bs_dir" || exit 99
        umask 022
        set -f
        US=$(printf '\037')
        while IFS= read -r line; do
            oldIFS=$IFS; IFS=$US
            set -- $line
            IFS=$oldIFS
            t="$1"
            case "$t" in
            ROOT) ;;
            D) mkdir -p "./$2"; chmod "$3" "./$2" 2>/dev/null ;;
            F)
                mkdir -p "$(dirname "./$2")"
                case "$3" in
                0) : > "./$2" ;;
                1) printf 'alpha\n' > "./$2" ;;
                2) seq 1 100 > "./$2" ;;
                3) dd if=/dev/zero of="./$2" bs=1024 count=8 2>/dev/null ;;
                *) printf 'content-%s\n' "$3" > "./$2" ;;
                esac
                chmod "$4" "./$2" 2>/dev/null
                touch -d '2021-01-01 00:00:00' "./$2"
                ;;
            L) mkdir -p "$(dirname "./$2")"; ln -s "$3" "./$2" 2>/dev/null
               touch -h -d '2021-01-01 00:00:00' "./$2" 2>/dev/null ;;
            H) mkdir -p "$(dirname "./$2")"; ln "./$3" "./$2" 2>/dev/null ;;
            S)
                mkdir -p "$(dirname "./$2")"
                : > "./$2"
                truncate -s "${3}K" "./$2" 2>/dev/null
                printf 'X' | dd of="./$2" bs=1 seek=$(( $3 * 512 )) \
                    conv=notrunc 2>/dev/null
                touch -d '2021-01-01 00:00:00' "./$2"
                ;;
            P) mkdir -p "$(dirname "./$2")"; mkfifo "./$2" 2>/dev/null ;;
            esac
        done < "$bs_plan"
        set +f
    )
}

fails=0
t=1
while [ "$t" -le "$trials" ]; do
    tdir="$work/t$t"
    mkdir -p "$tdir"
    gen_plan "$t" > "$tdir/plan"
    US=$(printf '\037')
    args=$(sed -n "s/^ARGS$US//p" "$tdir/plan")
    ans=$(sed -n 's/^STDIN //p' "$tdir/plan")

    # Pre-create captures OUTSIDE the sandboxes.
    for f in A.out A.err B.out B.err A.rc B.rc A.man B.man; do
        : > "$tdir/$f"
    done
    build_sandbox "$tdir/A" "$tdir/plan"
    build_sandbox "$tdir/B" "$tdir/plan"

    # Identity mode: serial vs parallel, worker count derived from
    # (seed, trial) so campaigns sweep the pool-size space.
    if [ "$identity" = 1 ]; then
        idw=$(( (seed * 7 + t * 13) % 15 + 2 ))
        a_env="CHOPIN_PARALLEL_WORKERS=0"
        b_env="CHOPIN_PARALLEL_MIN=1 CHOPIN_PARALLEL_WORKERS=$idw"
        if [ "${FUZZ_CHUNKS:-0}" = 1 ]; then
            b_env="$b_env CHOPIN_PARALLEL_CHUNKS=1"
            b_env="$b_env CHOPIN_CHUNK_THRESHOLD=65536"
        fi
    else
        a_env=""
        b_env=""
    fi

    for side in A B; do
        if [ "$side" = A ]; then tool="$oracle"; else tool="$root/chopin"; fi
        if [ "$side" = A ]; then side_env=$a_env; else side_env=$b_env; fi
        (
            cd "$tdir/$side" || exit 99
            umask 022
            set -f
            oldIFS=$IFS; IFS=$US
            # shellcheck disable=SC2086
            set -- $args
            IFS=$oldIFS
            # UNLIMITED identical answers: a single piped answer makes
            # multi-prompt trials diverge on traversal order (which
            # file consumed the answer differs) - `yes` keeps the
            # decision set order-independent.
            # shellcheck disable=SC2086
            if [ "$ans" = none ]; then
                env -i \
                    PATH="/usr/bin:/bin" HOME="$tdir/$side" \
                    LC_ALL=C LANGUAGE=C TZ=UTC0 \
                    CHOPIN_DEBUG_VERIFY=1 $side_env \
                    "$tool" "$@" < /dev/null
            else
                yes "$ans" 2>/dev/null | env -i \
                    PATH="/usr/bin:/bin" HOME="$tdir/$side" \
                    LC_ALL=C LANGUAGE=C TZ=UTC0 \
                    CHOPIN_DEBUG_VERIFY=1 $side_env \
                    "$tool" "$@"
            fi
        ) > "$tdir/$side.out" 2> "$tdir/$side.err"
        echo $? > "$tdir/$side.rc"
        "$manifest" "$tdir/$side" > "$tdir/$side.man" 2>/dev/null
    done

    ok=1
    [ "$(cat "$tdir/A.rc")" = "$(cat "$tdir/B.rc")" ] || ok=0
    cmp -s "$tdir/A.man" "$tdir/B.man" || ok=0
    if [ "$identity" = 1 ]; then
        # Same binary both sides: every byte in order, both streams.
        cp "$tdir/A.out" "$tdir/A.out.s"; cp "$tdir/B.out" "$tdir/B.out.s"
        cp "$tdir/A.err" "$tdir/A.err.s"; cp "$tdir/B.err" "$tdir/B.err.s"
        cmp -s "$tdir/A.out" "$tdir/B.out" || ok=0
        cmp -s "$tdir/A.err" "$tdir/B.err" || ok=0
    else
        sort "$tdir/A.out" > "$tdir/A.out.s"
        sort "$tdir/B.out" > "$tdir/B.out.s"
        cmp -s "$tdir/A.out.s" "$tdir/B.out.s" || ok=0
        # Normalize BEFORE sorting: prompt-splitting creates the lines
        # the sort must order (a glued prompt line sorts as one unit
        # otherwise - fuzz 1234-126).
        normprog < "$tdir/A.err" | apply_deviation_filter | sort \
            > "$tdir/A.err.s"
        normprog < "$tdir/B.err" | apply_deviation_filter | sort \
            > "$tdir/B.err.s"
        cmp -s "$tdir/A.err.s" "$tdir/B.err.s" || ok=0

        # Into-self partial trees are order-dependent (overview s2
        # corollary): when BOTH tools diagnosed into-itself, compare
        # only streams and rc.
        if [ "$ok" = 0 ] \
            && grep -q 'into itself' "$tdir/A.err" \
            && grep -q 'into itself' "$tdir/B.err"; then
            ok=1
            [ "$(cat "$tdir/A.rc")" = "$(cat "$tdir/B.rc")" ] || ok=0
            cmp -s "$tdir/A.out.s" "$tdir/B.out.s" || ok=0
            cmp -s "$tdir/A.err.s" "$tdir/B.err.s" || ok=0
        fi
    fi

    if [ "$ok" = 0 ]; then
        fails=$((fails + 1))
        mkdir -p "$faildir"
        rm -rf "$faildir/trial-$seed-$t"
        chmod -R u+rwx "$tdir" 2>/dev/null
        cp -r "$tdir" "$faildir/trial-$seed-$t"
        echo "FUZZ FAIL trial $t (seed $seed): artifacts in $faildir/trial-$seed-$t"
        echo "  args: $args"
        diff "$tdir/A.err.s" "$tdir/B.err.s" | head -4
        diff "$tdir/A.man" "$tdir/B.man" | head -4
    else
        rmtree "$tdir"
    fi
    t=$((t + 1))
done

mode=fuzz
[ "$identity" = 1 ] && mode=identity
[ "${FUZZ_CHUNKS:-0}" = 1 ] && mode="$mode+chunks"
echo "$mode: $trials trials, $fails failures (seed $seed)"
[ "$fails" -eq 0 ] || exit 1
exit 0
