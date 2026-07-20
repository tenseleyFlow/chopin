#!/bin/sh
# chopin unit tier: SRC-drift check, binary smoke, manifest tool
# self-tests. Option-resolution tables arrive in sprint 01.

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

# --- Manifest tool: build with the strict flag set, then self-test.
cc_bin=$(sed -n 's/^CC ?= //p' config.mk)
extra=$(sed -n 's/^EXTRA_CPPFLAGS = //p' config.mk)
mkdir -p build
if ! "$cc_bin" -std=c11 -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 $extra \
    -I. -Wall -Wextra -Werror -o build/manifest tests/manifest.c; then
    bad "manifest tool does not compile -Werror clean"
else
    note "ok: manifest tool compiles"
    work=$(mktemp -d "${TMPDIR:-/tmp}/chopin-unit.XXXXXX")
    trap 'chmod -R u+rwx "$work" 2>/dev/null; rm -rf "$work"' EXIT

    mktree() {
        mkdir -p "$1/sub"
        printf 'alpha\n' > "$1/a.txt"
        printf 'beta\n' > "$1/sub/b.txt"
        ln "$1/a.txt" "$1/hard.txt"
        ln -s a.txt "$1/link"
        ln -s ../escape "$1/dangle"
        : > "$1/sub/with space"
        mkfifo "$1/fifo"
        # 1 MiB sparse tail; both trees get the same shape.
        printf 'x' > "$1/sparse.bin"
        truncate -s 1M "$1/sparse.bin" 2>/dev/null || {
            dd if=/dev/zero of="$1/sparse.bin" bs=1 count=1 \
                seek=1048575 2>/dev/null
        }
    }
    mktree "$work/t1"
    mktree "$work/t2"

    build/manifest "$work/t1" > "$work/m1" || bad "manifest t1 exited nonzero"
    build/manifest "$work/t2" > "$work/m2" || bad "manifest t2 exited nonzero"
    if cmp -s "$work/m1" "$work/m2"; then
        note "ok: identical trees -> identical manifests"
    else
        bad "identical trees produced differing manifests"
        diff "$work/m1" "$work/m2" | head -10
    fi

    # 1-bit content change -> exactly the changed file's line differs.
    printf 'Alpha\n' > "$work/t2/a.txt"
    build/manifest "$work/t2" > "$work/m3" || bad "manifest t2v2 nonzero"
    if cmp -s "$work/m1" "$work/m3"; then
        bad "content change did not change the manifest"
    else
        changed=$(diff "$work/m1" "$work/m3" | grep -c '^[<>]')
        # a.txt and hard.txt share the inode: both lines' HASH move.
        if [ "$changed" -eq 4 ]; then
            note "ok: 1-bit change moves exactly the linked lines' HASH"
        else
            bad "unexpected diff shape after content change ($changed lines)"
            diff "$work/m1" "$work/m3"
        fi
    fi

    grep -q '^hard.txt f .* a.txt - -$' "$work/m1" \
        && note "ok: hardlink class points at first-seen path" \
        || { bad "hardlink class wrong"; grep '^hard' "$work/m1"; }
    grep -q '^link l .* - a.txt -$' "$work/m1" \
        && note "ok: symlink target recorded" \
        || { bad "symlink line wrong"; grep '^link' "$work/m1"; }
    grep -q '^sub/with%20space f ' "$work/m1" \
        && note "ok: hostile name percent-encoded" \
        || bad "space-name not encoded"
    grep -q '^sparse.bin f .* S$' "$work/m1" \
        && note "ok: sparse class detected" \
        || { bad "sparse class missing"; grep '^sparse' "$work/m1"; }
    grep -q '^fifo p ' "$work/m1" \
        && note "ok: fifo typed p" || bad "fifo line wrong"

    # -t appends an mtime column for f/d/l.
    build/manifest -t "$work/t1" > "$work/mt" || bad "manifest -t nonzero"
    grep -q '^a.txt f .* [0-9][0-9]*\.[0-9]\{9\}$' "$work/mt" \
        && note "ok: -t emits mtime column" \
        || { bad "-t mtime column wrong"; grep '^a.txt' "$work/mt"; }

    # -x digests user.* xattrs where the tools and fs allow.
    if command -v setfattr >/dev/null 2>&1 \
        && setfattr -n user.chopin -v hello "$work/t1/a.txt" 2>/dev/null; then
        build/manifest -x "$work/t1" > "$work/mx" || bad "manifest -x nonzero"
        grep -q '^a.txt f .* user.chopin=[0-9a-f]\{16\}$' "$work/mx" \
            && note "ok: -x digests user.* xattrs" \
            || { bad "-x xattr digest wrong"; grep '^a.txt' "$work/mx"; }
    else
        note "skip: xattr self-test (no setfattr or fs support)"
    fi

    # Determinism: a second run is byte-identical.
    build/manifest "$work/t1" > "$work/m1b" || bad "manifest rerun nonzero"
    cmp -s "$work/m1" "$work/m1b" \
        && note "ok: manifest run is deterministic" \
        || bad "manifest nondeterministic across runs"
fi

if [ "$fail" -ne 0 ]; then
    echo "unit tier: FAIL"
    exit 1
fi
echo "unit tier: PASS"
