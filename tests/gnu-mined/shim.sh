# Stand-in for coreutils tests/init.sh: the framework functions the
# mined tests/cp scripts consume (replacements-analysis.md s3 is the
# catalog). Sourced as $srcdir/tests/init.sh from a per-script
# sandbox. Snapshot oddities (missing Exit, misspellings) are
# documented in the catalog and never silently fixed here.

fail=0

framework_failure_() { echo "framework_failure_ $*" >&2; exit 99; }
print_ver_() { :; }
path_prepend_() { :; }  # the runner controls PATH
skip_() { echo "skip: $*" >&2; exit 77; }
fail_() { echo "fail: $*" >&2; fail=1; }

compare() {
    diff -u "$1" "$2"
}

returns_() {
    r_want=$1; shift
    "$@"
    r_got=$?
    test "$r_got" = "$r_want"
}

Exit() { exit "$1"; }

# init.sh retry_delay_: call TESTFUNC with a delay argument, doubling
# up to MAXTRIES attempts (time-sensitive tests). TESTFUNC returns 0
# when the probe succeeded.
retry_delay_() {
    rd_fn="$1"; rd_delay="$2"; rd_tries="$3"
    rd_i=0
    while :; do
        "$rd_fn" "$rd_delay" && return 0
        rd_i=$((rd_i + 1))
        [ "$rd_i" -ge "$rd_tries" ] && return 1
        rd_delay=$(awk "BEGIN{print $rd_delay*2}")
    done
}

# Diagnostics channel some scripts write to.
exec 9>&2
