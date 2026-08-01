#!/bin/sh
# bench/tabulate.sh - render one comparison table from a run's
# summary.tsv (sprint 11A). Ratios are chopin-relative:
#   vs GNU  = gnu / chopin   (>1.00 means chopin is faster)
#   vs best rival likewise, naming the rival.
# Lanes whose result-tree manifest did not match the source are
# reported as no-fidelity and NEVER counted as a rival win: a copy
# that copied wrong is not a faster copy.
#
# Usage: sh bench/tabulate.sh <summary.tsv> [label]

set -eu
sum=${1:?usage: tabulate.sh <summary.tsv> [label]}
label=${2:-$(basename "$(dirname "$sum")")}

[ -r "$sum" ] || { echo "tabulate: cannot read $sum" >&2; exit 1; }

printf '## %s\n\n' "$label"
printf '%-20s %10s %10s %8s   %-22s %8s\n' \
    lane chopin gnu 'vs GNU' 'best rival' 'vs rival'
printf '%-20s %10s %10s %8s   %-22s %8s\n' \
    -------------------- ---------- ---------- -------- \
    ---------------------- --------

awk -F'\t' '
{
    lane = $1; tool = $2; val = $3
    if (val == "TREE-MISMATCH") { nofid[lane "/" tool] = 1; next }
    if (val == "FAIL") { failed[lane "/" tool] = 1; next }
    t[lane "/" tool] = val + 0
    if (!(lane in seen)) { order[++n] = lane; seen[lane] = 1 }
}
END {
    for (i = 1; i <= n; i++) {
        lane = order[i]
        c = t[lane "/chopin"]
        if (c == 0) continue
        g = t[lane "/gnu"]

        gr = (g > 0) ? sprintf("%.2fx", g / c) : "-"

        bestname = "-"; best = 0
        split("fcp xcp wcp", rivals, " ")
        for (r = 1; r <= 3; r++) {
            rv = t[lane "/" rivals[r]]
            if (rv <= 0) continue
            # A no-fidelity result is recorded but never treated as a bar.
            if ((lane "/" rivals[r]) in nofid) {
                if (bestname == "-") bestname = rivals[r] " (no-fidelity)"
                continue
            }
            if (best == 0 || rv < best) { best = rv; bestname = rivals[r] }
        }
        rr = (best > 0) ? sprintf("%.2fx", best / c) : "-"
        if (best > 0) bestname = sprintf("%s %.4g", bestname, best)

        printf "%-20s %10.4g %10.4g %8s   %-22s %8s\n",
            lane, c, (g > 0 ? g : 0), gr, bestname, rr
    }
}' "$sum"

printf '\nRatios are chopin-relative: 1.50x means chopin took 2/3 the time.\n'
printf 'Below 1.00x means chopin was SLOWER.\n'
