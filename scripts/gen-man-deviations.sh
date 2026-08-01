#!/bin/sh
# scripts/gen-man-deviations.sh - render doc/deviations.md's register
# entries as roff for chopin.1's DEVIATIONS section. One source of
# truth (sprint 11 locked decision): the man page never carries a
# hand-written copy of the register.
#
# Reads doc/deviations.md, writes roff to stdout. `make man` splices
# the output between the DEVIATIONS markers in doc/chopin.1.

set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
reg="$root/doc/deviations.md"

[ -r "$reg" ] || { echo "gen-man-deviations: no $reg" >&2; exit 1; }

awk '
/^## DEV-/ {
    # "## DEV-001 title words [CLASS - trailing]"
    line = substr($0, 4)
    id = $2
    cls = "UNCLASSED"
    if (match(line, /\[[^]]*\]/)) {
        cls = substr(line, RSTART + 1, RLENGTH - 2)
        # Strip the schedule/scope trailer only: " - lands sprint 03",
        # " v0.1". The class itself may contain a hyphen
        # (QUIRK-KEPT, BUILD-PARITY) and must survive intact.
        sub(/ +- .*$/, "", cls)
        sub(/ +v[0-9].*$/, "", cls)
        title = substr(line, 1, RSTART - 1)
    } else {
        title = line
    }
    sub(/^[^ ]+ /, "", title)           # drop the ID
    sub(/ +$/, "", title)
    printf ".TP\n.B %s (%s)\n%s.\n", id, cls, title
    body = 1
    blank = 0
    next
}
/^## / { body = 0 }
body == 1 {
    if ($0 ~ /^ *$/) { blank = 1; next }
    # Register prose is already wrapped; roff re-fills. Escape the
    # roff comment/control leader so a line-initial "." or "\" cannot
    # inject a request.
    l = $0
    gsub(/\\/, "\\e", l)
    if (substr(l, 1, 1) == ".") l = "\\&" l
    if (blank == 1) { printf ".sp\n"; blank = 0 }
    print l
}
' "$reg"
