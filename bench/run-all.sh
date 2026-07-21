#!/bin/sh
# bench/run-all.sh - the full lane matrix (sprint 10). Results land
# under bench/results/<host>-<date>/ (untracked); the tables that
# ship are copied into the release artifacts by sprint 11.
#
# BENCH_SCALE=release for the locked lane sizes; BENCH_LANES to run
# a subset ("swarm large-single"); BENCH_TOOLS to restrict tools.

set -u
. "$(dirname -- "$0")/lib.sh"

lanes="${BENCH_LANES:-swarm swarm-empty kernel-tree large-single sparse hardlink-farm metadata-heavy reflink smallfile}"
tools="${BENCH_TOOLS:-chopin gnu fcp xcp wcp}"

machine_info
: > "$resdir/summary.tsv"
echo "bench: scale=$scale runs=$runs -> $resdir"

tool_cmd() {
    # tool_cmd <tool> <src> <dstdir> ; echoes nothing & fails if the
    # tool is absent. -R semantics normalized across tools.
    case $1 in
    chopin) echo "$CHOPIN -R $2 $3" ;;
    chopin-serial) echo "env CHOPIN_PARALLEL_WORKERS=0 $CHOPIN -R $2 $3" ;;
    gnu)    echo "$GNU -R $2 $3" ;;
    fcp)    [ -x "$FCP" ] && echo "$FCP $2 $3" ;;
    xcp)    [ -x "$XCP" ] && echo "$XCP -r $2 $3" ;;
    wcp)    [ -x "$WCP" ] && echo "$WCP $2 $3" ;;
    esac
}

archive_lane() {   # -a semantics for the metadata lane
    case $1 in
    chopin) echo "$CHOPIN -a $2 $3" ;;
    gnu)    echo "$GNU -a $2 $3" ;;
    *)      return 1 ;;   # rivals lack full -a fidelity; not their lane
    esac
}

for lane in $lanes; do
    echo "== $lane"
    fix=$(sh "$bench_root/bench/fixtures.sh" "$lane") || {
        echo "  fixture failed; skipping"; continue; }
    src="$fix/src"
    dst="$fix/dst"

    case $lane in
    swarm|kernel-tree)      temp=cold ;;
    large-single)           temp=both ;;
    *)                      temp=warm ;;
    esac

    for tool in $tools; do
        if [ "$lane" = metadata-heavy ]; then
            cmd=$(archive_lane "$tool" "$src" "$dst") || continue
        else
            cmd=$(tool_cmd "$tool" "$src" "$dst")
        fi
        [ -n "$cmd" ] || continue
        if [ "$temp" = both ]; then
            bench_lane "$lane-warm" warm "$src" "$dst" "$tool" $cmd
            bench_lane "$lane-cold" cold "$src" "$dst" "$tool" $cmd
        else
            bench_lane "$lane" "$temp" "$src" "$dst" "$tool" $cmd
        fi
    done
done

echo
echo "== summary ($resdir/summary.tsv)"
column -t "$resdir/summary.tsv" 2>/dev/null || cat "$resdir/summary.tsv"
