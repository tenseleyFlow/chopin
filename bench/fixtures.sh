#!/bin/sh
# bench/fixtures.sh - seeded, cached fixture builders (sprint 10A).
# Shape is seeded (awk LCG) so every machine builds the same tree;
# content bytes come from urandom (timing is shape-driven). Cached
# under build/bench with .done stamps; SCALE=release uses the locked
# lane sizes.
#
# Usage: sh bench/fixtures.sh <name>   (swarm, swarm-empty,
#        kernel-tree, large-single, sparse, hardlink-farm,
#        metadata-heavy, reflink, smallfile)

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixdir="$here/build/bench"
scale="${BENCH_SCALE:-dev}"
mkdir -p "$fixdir"

# awk LCG shared by shaped builders (declared OUTSIDE rule blocks -
# awk functions cannot live inside BEGIN).
shape_awk='function rnd() { s = (s * 1103515245 + 12345) % 2147483648; return s / 2147483648 }'

build_swarm() {   # N x 1KiB tree, 256 dirs - shape is deterministic
    n=$1; d="$fixdir/swarm-$n"
    [ -f "$d/.done" ] && { echo "$d"; return; }
    rm -rf "$d"; mkdir -p "$d/src"
    i=0
    while [ $i -lt 256 ]; do
        mkdir "$d/src/$(printf 'd%03d' $i)"; i=$((i+1))
    done
    head -c 1024 /dev/urandom > "$d/.blk"
    awk -v n="$n" 'BEGIN { for (i = 0; i < n; i++)
        printf "src/d%03d/f%d\n", i % 256, i }' |
    while IFS= read -r p; do cp "$d/.blk" "$d/$p"; done
    rm "$d/.blk"
    : > "$d/.done"; echo "$d"
}

build_swarm_empty() {   # N x 0-byte - pure dispatch overhead
    n=$1; d="$fixdir/swarm-empty-$n"
    [ -f "$d/.done" ] && { echo "$d"; return; }
    rm -rf "$d"; mkdir -p "$d/src"
    i=0
    while [ $i -lt 512 ]; do mkdir "$d/src/d$i"; i=$((i+1)); done
    awk -v n="$n" 'BEGIN { for (i = 0; i < n; i++)
        printf "src/d%d/f%d\n", i % 512, i }' |
    (cd "$d" && xargs touch)
    : > "$d/.done"; echo "$d"
}

build_kernel_tree() {   # 65k files, linux-checkout size distribution
    n=$1; d="$fixdir/kernel-tree-$n"
    [ -f "$d/.done" ] && { echo "$d"; return; }
    rm -rf "$d"; mkdir -p "$d/src"
    # Size model: ~55% < 4K, 30% 4-16K, 12% 16-128K, 3% 128K-1M
    # (median ~3K, mean ~11K - a linux tree's shape).
    awk -v n="$n" "$shape_awk"'
    BEGIN { s = 1234
        for (i = 0; i < n; i++) {
            dir = int(rnd() * 4500)
            r = rnd()
            if (r < 0.55)      sz = int(200 + rnd() * 3800)
            else if (r < 0.85) sz = int(4096 + rnd() * 12288)
            else if (r < 0.97) sz = int(16384 + rnd() * 114688)
            else               sz = int(131072 + rnd() * 917504)
            printf "%d src/k%02d/s%03d/f%d\n", sz, dir % 64, dir, i }
    }' > "$d/.plan"
    cut -d' ' -f2 "$d/.plan" | sed 's|/[^/]*$||' | sort -u |
    while IFS= read -r p; do mkdir -p "$d/$p"; done
    head -c 1048576 /dev/urandom > "$d/.pool"
    while read -r sz p; do
        head -c "$sz" "$d/.pool" > "$d/$p"
    done < "$d/.plan"
    rm "$d/.plan" "$d/.pool"
    : > "$d/.done"; echo "$d"
}

build_large_single() {   # one dense multi-GiB file
    mb=$1; d="$fixdir/large-$mb"
    [ -f "$d/.done" ] && { echo "$d"; return; }
    rm -rf "$d"; mkdir -p "$d/src"
    dd if=/dev/urandom of="$d/src/big" bs=1M count="$mb" status=none
    : > "$d/.done"; echo "$d"
}

build_sparse() {   # VM-image shape: mostly holes, scattered extents
    mb=$1; d="$fixdir/sparse-$mb"
    [ -f "$d/.done" ] && { echo "$d"; return; }
    rm -rf "$d"; mkdir -p "$d/src"
    truncate -s "${mb}M" "$d/src/vm.img"
    awk -v mb="$mb" "$shape_awk"'
    BEGIN { s = 777
        n = mb / 4
        for (i = 0; i < n; i++)
            if (rnd() < 0.15) printf "%d\n", i }' |
    while IFS= read -r blk; do
        dd if=/dev/urandom of="$d/src/vm.img" bs=1M count=4 \
           seek=$((blk * 4)) conv=notrunc status=none
    done
    : > "$d/.done"; echo "$d"
}

build_hardlink_farm() {   # dense link groups: n inodes x 8 links
    n=$1; d="$fixdir/hardlink-$n"
    [ -f "$d/.done" ] && { echo "$d"; return; }
    rm -rf "$d"; mkdir -p "$d/src"
    i=0
    while [ $i -lt 64 ]; do mkdir "$d/src/g$i"; i=$((i+1)); done
    head -c 2048 /dev/urandom > "$d/.blk"
    awk -v n="$n" 'BEGIN { for (i = 0; i < n; i++)
        printf "%d\n", i }' |
    while IFS= read -r i; do
        g=$((i % 64))
        cp "$d/.blk" "$d/src/g$g/a$i"
        for l in b c d e f g h; do
            ln "$d/src/g$g/a$i" "$d/src/g$g/$l$i"
        done
    done
    rm "$d/.blk"
    : > "$d/.done"; echo "$d"
}

build_metadata_heavy() {   # deep tree, xattrs, varied modes/times
    n=$1; d="$fixdir/meta-$n"
    [ -f "$d/.done" ] && { echo "$d"; return; }
    rm -rf "$d"; mkdir -p "$d/src"
    head -c 512 /dev/urandom > "$d/.blk"
    awk -v n="$n" "$shape_awk"'
    BEGIN { s = 5150
        for (i = 0; i < n; i++)
            printf "src/a%d/b%d/c%d/f%d %d\n",
                i % 16, int(i / 16) % 16, int(i / 256) % 16, i,
                int(rnd() * 3) }' |
    while read -r p nx; do
        mkdir -p "$d/${p%/*}"
        cp "$d/.blk" "$d/$p"
        chmod 6$((44 + nx))4 "$d/$p" 2>/dev/null || chmod 644 "$d/$p"
        j=0
        while [ "$j" -le "$nx" ]; do
            setfattr -n "user.bench$j" -v "v$j-$p" "$d/$p" 2>/dev/null || break
            j=$((j+1))
        done
    done
    rm "$d/.blk"
    : > "$d/.done"; echo "$d"
}

build_reflink() {   # FICLONE latency: many mid-size files on CoW fs
    n=$1; d="$fixdir/reflink-$n"
    [ -f "$d/.done" ] && { echo "$d"; return; }
    rm -rf "$d"; mkdir -p "$d/src"
    head -c 65536 /dev/urandom > "$d/.blk"
    i=0
    while [ $i -lt "$n" ]; do
        mkdir -p "$d/src/r$((i % 32))"
        cp "$d/.blk" "$d/src/r$((i % 32))/f$i"
        i=$((i+1))
    done
    rm "$d/.blk"
    : > "$d/.done"; echo "$d"
}

build_smallfile() {   # startup-floor guard: one 4K file
    d="$fixdir/smallfile"
    [ -f "$d/.done" ] && { echo "$d"; return; }
    rm -rf "$d"; mkdir -p "$d/src"
    head -c 4096 /dev/urandom > "$d/src/one"
    : > "$d/.done"; echo "$d"
}

if [ "$scale" = release ]; then
    SWARM_N=200000; EMPTY_N=1000000; KTREE_N=65000
    LARGE_MB=4096; SPARSE_MB=2048; HL_N=5000; META_N=20000; RL_N=10000
else
    SWARM_N=50000; EMPTY_N=200000; KTREE_N=65000
    LARGE_MB=1024; SPARSE_MB=512; HL_N=2000; META_N=8000; RL_N=4000
fi

case "${1:-}" in
swarm)          build_swarm "$SWARM_N" ;;
swarm-empty)    build_swarm_empty "$EMPTY_N" ;;
kernel-tree)    build_kernel_tree "$KTREE_N" ;;
large-single)   build_large_single "$LARGE_MB" ;;
sparse)         build_sparse "$SPARSE_MB" ;;
hardlink-farm)  build_hardlink_farm "$HL_N" ;;
metadata-heavy) build_metadata_heavy "$META_N" ;;
reflink)        build_reflink "$RL_N" ;;
smallfile)      build_smallfile ;;
*) echo "usage: fixtures.sh <lane>" >&2; exit 2 ;;
esac
