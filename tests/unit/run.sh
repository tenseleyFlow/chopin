#!/bin/sh
# chopin unit tier. Compiled drivers arrive with the manifest tool (00B)
# and the option-resolution tables (sprint 01); the SRC-drift check and
# the binary smoke assertions run from the first commit.

set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
cd "$root"

fail=0
note() { printf '%s\n' "$*"; }
bad() { printf 'FAIL: %s\n' "$*"; fail=1; }

# --- SRC drift: every src/**/*.c must be in the Makefile SRC list and
# vice versa (liszt tests/unit/run.sh:779-781 pattern).
makefile_src=$(sed -n '/^SRC = /,/^$/p' Makefile | sed 's/^SRC = //' \
    | tr -d '\\' | tr ' \t' '\n\n' | grep '\.c$' | sort)
disk_src=$(find src -name '*.c' | sort)
if [ "$makefile_src" != "$disk_src" ]; then
    bad "Makefile SRC drifted from src/ contents"
    printf 'Makefile:\n%s\ndisk:\n%s\n' "$makefile_src" "$disk_src"
else
    note "ok: SRC list matches src/ tree"
fi

# --- Binary smoke: version prints, program name follows argv[0].
v=$(./chopin --version) || bad "chopin --version exited nonzero"
case "$v" in
"chopin "*) note "ok: chopin --version -> $v" ;;
*) bad "unexpected --version output: $v" ;;
esac

vc=$(./cpn --version) || bad "cpn --version exited nonzero"
case "$vc" in
"cpn "*) note "ok: cpn --version -> $vc" ;;
*) bad "cpn does not report its own name: $vc" ;;
esac

# --- Skeleton refuses real work with exit 1.
if ./chopin a b 2>/dev/null; then
    bad "skeleton chopin a b should exit 1"
else
    note "ok: skeleton refuses copy with exit 1"
fi

if [ "$fail" -ne 0 ]; then
    echo "unit tier: FAIL"
    exit 1
fi
echo "unit tier: PASS"
