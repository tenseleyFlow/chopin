#!/bin/sh
# GNU tests/cp mined tier (sprint 03C). Each mined script runs
# unmodified against BOTH the oracle and chopin (each invoked through
# a bindir symlink named `cp`, so argv[0]-derived diagnostics read
# "cp:" for both). A script is MINED when chopin passes it, or a
# documented dev-tier exception applies (documented = register entry;
# the exception table below names the ID). Anything else fails the
# tier. Exit: 0 green, 1 failures, 77 skip (no pinned oracle / root).
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
cd "$root"

if [ "$(id -u)" = 0 ]; then
    echo "gnu-mined: SKIPPED - refusing to run as root" >&2
    exit 77
fi

oracle=$(sh scripts/find-gnu-cp.sh) || {
    echo "gnu-mined: no oracle; skipping (77)" >&2
    exit 77
}
case $("$oracle" --version | sed -n 1p) in
*" 9.11") ;;
*) echo "gnu-mined: oracle not the 9.11 pin; skipping (77)" >&2; exit 77 ;;
esac
if [ "$oracle" != "$root/build/gnu-cp/src/cp" ] \
    && [ "${CHOPIN_ORACLE_STRICT:-}" != 1 ]; then
    echo "gnu-mined: oracle not the feature-pinned build; skipping (77)" >&2
    exit 77
fi

corpus="$root/.docs/refs/gnu-coreutils/tests/cp"
[ -d "$corpus" ] || {
    echo "gnu-mined: corpus absent (.docs is local-only); skipping (77)" >&2
    exit 77
}

work="$root/tests/.work/gnu-mined"
chmod -R u+rwx "$work" 2>/dev/null || true
rm -rf "$work"
mkdir -p "$work"
trap 'chmod -R u+rwx "$work" 2>/dev/null; rm -rf "$work"' EXIT

# Mined set (sandbox-safe, no root/SELinux/mv, no -R yet).
scripts="backup-1 backup-is-src cp-mv-backup preserve-mode acl"

# script -> sprint whose machinery it waits for (DEFER, not failure).
deferred_until() {
    case "$1" in
    preserve-mode) echo "06" ;;   # uses cp -r mid-script
    acl) echo "06" ;;             # setfacl vs the ACL-disabled oracle;
                                  # xattr-parity behavior is golden-
                                  # covered (meta-xattr); revisit with
                                  # the fs job lanes
    *) echo "" ;;
    esac
}

# script -> register ID whose fix legitimately fails it.
expected_deviation() {
    case "$1" in
    backup-is-src) echo DEV-001 ;;   # pins GNU's double-space message
    *) echo "" ;;
    esac
}

run_script() {
    rs_script="$1"; rs_tool="$2"; rs_label="$3"
    d="$work/$rs_script.$rs_label"
    mkdir -p "$d/tests" "$d/bin" "$d/sandbox"
    cp tests/gnu-mined/shim.sh "$d/tests/init.sh"
    ln -s "$rs_tool" "$d/bin/cp"
    (
        cd "$d/sandbox" || exit 99
        umask 022
        env -i \
            PATH="$d/bin:/usr/bin:/bin" \
            HOME="$d/sandbox" \
            LC_ALL=C LANGUAGE=C TZ=UTC0 \
            srcdir="$d" \
            sh "$corpus/$rs_script.sh"
    ) > "$d/log" 2>&1
    echo $?
}

mined=0
deviation=0
skipped=0
failed=0

deferred=0
for s in $scripts; do
    d=$(deferred_until "$s")
    if [ -n "$d" ]; then
        echo "DEFER $s (sprint $d)"
        deferred=$((deferred + 1))
        continue
    fi
    orc=$(run_script "$s" "$oracle" oracle)
    crc=$(run_script "$s" "$root/chopin" chopin)

    if [ "$orc" = 77 ]; then
        echo "SKIP  $s (oracle skipped)"
        skipped=$((skipped + 1))
        continue
    fi
    if [ "$orc" != 0 ]; then
        echo "FAIL  $s: ORACLE failed (rc=$orc) - shim gap?"
        sed 's/^/    /' "$work/$s.oracle/log" | head -8
        failed=$((failed + 1))
        continue
    fi
    if [ "$crc" = 0 ]; then
        echo "MINED $s"
        mined=$((mined + 1))
    else
        dev=$(expected_deviation "$s")
        if [ -n "$dev" ] && grep -q "^## $dev " "$root/doc/deviations.md"; then
            echo "DEV   $s (expected: $dev)"
            deviation=$((deviation + 1))
        else
            echo "FAIL  $s: chopin rc=$crc with no documented exception"
            sed 's/^/    /' "$work/$s.chopin/log" | head -8
            failed=$((failed + 1))
        fi
    fi
done

echo "gnu-mined: $mined mined, $deviation deviation-documented, $deferred deferred, $skipped skipped, $failed failed"
[ "$failed" -eq 0 ] || exit 1
exit 0
