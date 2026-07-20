#!/bin/sh
# Print the path of a GNU coreutils cp oracle on stdout; exit 1 if none.
# Preference: $CHOPIN_ORACLE, the pinned local build, gcp (brew/macports
# coreutils), cp - exact-pin versions first, then any GNU coreutils cp
# (callers may warn on vintage drift). BSD/busybox cp is never accepted.
# Note only the pinned local build carries the pinned feature set; the
# harness treats any other match as PARITY_ACTIVE=maybe (00C).
set -u

pin="9.11"

version_line() {
    "$1" --version 2>/dev/null | sed -n 1p
}

resolve() {
    case "$1" in
    /*) [ -x "$1" ] && printf '%s\n' "$1" ;;
    */*) [ -x "$1" ] && printf '%s/%s\n' "$(pwd)" "$1" ;;
    *) command -v "$1" 2>/dev/null ;;
    esac
}

candidates="${CHOPIN_ORACLE:-} build/gnu-cp/src/cp gcp cp"

best=""
for cand in $candidates; do
    path=$(resolve "$cand") || continue
    [ -n "$path" ] || continue
    line=$(version_line "$path")
    case "$line" in
    *"GNU coreutils"*) ;;
    *) continue ;;
    esac
    case "$line" in
    *" $pin") printf '%s\n' "$path"; exit 0 ;;
    esac
    [ -n "$best" ] || best="$path"
done

if [ -n "$best" ]; then
    printf '%s\n' "$best"
    exit 0
fi
exit 1
