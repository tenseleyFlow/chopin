#!/bin/sh
# scripts/check-register.sh - the deviation register must be fully
# dispositioned before a release (sprint 11 exit criterion).
#
# For every DEV-nnn entry in doc/deviations.md:
#   - it has a class ([FIX], [QUIRK-KEPT], [BUILD-PARITY]);
#   - FIX entries name their pin ("Pinned by:") and their upstream
#     disposition ("Upstream:");
#   - every FIX has a draft section in the upstream-reports audit -
#     LOCAL ONLY: .docs/ is planning material and is never tracked in
#     the remote, so this leg is skipped where the audit is absent
#     (the tracked register's "Upstream:" line is the source of truth
#     that ships);
#   - the fuzz deviation filter mentions no ID the register lacks.

set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
reg="$root/doc/deviations.md"
drafts="$root/.docs/audits/upstream-reports.md"
filter="$root/tests/golden/DEVIATIONS"

fails=0
bad() { echo "register: $*" >&2; fails=$((fails + 1)); }

ids=$(sed -n 's/^## \(DEV-[0-9]*\) .*/\1/p' "$reg")
[ -n "$ids" ] || { echo "register: no entries found" >&2; exit 1; }

for id in $ids; do
    line=$(grep "^## $id " "$reg")
    case $line in
    *"[FIX"*|*"[QUIRK-KEPT"*|*"[BUILD-PARITY"*) ;;
    *) bad "$id has no recognized class" ;;
    esac

    body=$(awk -v id="$id" '
        $0 ~ "^## " id " " { on = 1; next }
        /^## / { on = 0 }
        on == 1 { print }' "$reg")

    case $line in
    *"[FIX"*)
        case $body in
        *"Pinned by:"*) ;;
        *) bad "$id (FIX) names no pin" ;;
        esac
        case $body in
        *"Upstream:"*) ;;
        *) bad "$id (FIX) has no upstream disposition" ;;
        esac
        if [ -r "$drafts" ]; then
            grep -q "^## $id" "$drafts" \
                || bad "$id (FIX) has no upstream draft section"
        fi
        ;;
    esac
done

# No filter line may reference an ID the register does not define.
if [ -r "$filter" ]; then
    for fid in $(sed -n 's/.*\(DEV-[0-9]\{3\}\).*/\1/p' "$filter" | sort -u); do
        printf '%s\n' "$ids" | grep -qx "$fid" \
            || bad "DEVIATIONS filter references unknown $fid"
    done
fi

n=$(printf '%s\n' "$ids" | grep -c .)
if [ "$fails" -eq 0 ]; then
    if [ -r "$drafts" ]; then
        echo "register: $n entries, all dispositioned (drafts cross-checked)"
    else
        echo "register: $n entries, all dispositioned (drafts not present)"
    fi
    exit 0
fi
echo "register: $fails problem(s)" >&2
exit 1
