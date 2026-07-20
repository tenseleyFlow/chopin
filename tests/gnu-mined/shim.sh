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

# Diagnostics channel some scripts write to.
exec 9>&2
