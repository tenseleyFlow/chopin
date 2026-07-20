#!/bin/sh
# Build the pinned GNU coreutils cp into build/gnu-cp/src/cp and print its
# path. Source preference: local .docs/refs/gnu-coreutils (the exact 9.11
# release tree), else the release tarball fetched from ftp.gnu.org
# (sha256-pinned). Out-of-tree build keeps the refs copy pristine.
#
# ORACLE FEATURE SET IS PINNED (sprint 00): --disable-nls (untranslated
# diagnostics ARE the parity target), --without-selinux, --disable-acl,
# xattr ON (USE_XATTR must land 1 - install the attr dev package if the
# probe fails). The configure summary is recorded to
# build/gnu-cp/oracle-features.txt and the build is REFUSED on any
# feature drift: golden results must not vary by build host.
set -eu

pin="9.11"
sha256_pin=394024eda0a5955217ceda9cd1201e65dc8fa3aa29c2951135a49521d57c3cc3
out="build/gnu-cp"
bin="$out/src/cp"

if [ -x "$bin" ] && "$bin" --version | sed -n 1p | grep -q " $pin\$" \
    && [ -f "$out/oracle-features.txt" ]; then
    printf '%s\n' "$bin"
    exit 0
fi

src=".docs/refs/gnu-coreutils"
if [ -f "$src/configure" ]; then
    # A git checkout scrambles mtimes; configure.ac "newer" than its
    # outputs triggers maintainer-mode automake regen and dies (tally's
    # lesson). Touch generated files in dependency order, aclocal first.
    touch "$src/aclocal.m4" 2>/dev/null || true
    sleep 1 # strict ordering on 1s-granularity filesystems
    find "$src" -name configure -o -name config.hin -o -name Makefile.in \
        | xargs touch 2>/dev/null || true
else
    mkdir -p build
    tarball="build/coreutils-$pin.tar.xz"
    url="https://ftp.gnu.org/gnu/coreutils/coreutils-$pin.tar.xz"
    if [ ! -f "$tarball" ]; then
        if command -v curl >/dev/null 2>&1; then
            curl -fsSL -o "$tarball" "$url"
        elif command -v fetch >/dev/null 2>&1; then
            fetch -o "$tarball" "$url"
        else
            wget -qO "$tarball" "$url"
        fi
    fi
    got=$( (sha256sum "$tarball" 2>/dev/null || sha256 -q "$tarball" | sed 's/$/  x/') | awk '{print $1}')
    [ "$got" = "$sha256_pin" ] || {
        echo "build-gnu-cp: tarball sha256 mismatch ($got)" >&2
        exit 1
    }
    tar xf "$tarball" -C build
    src="build/coreutils-$pin"
fi

srcabs=$(cd "$src" && pwd)
mkdir -p "$out"
outabs=$(cd "$out" && pwd)

jobs=$( (nproc || sysctl -n hw.ncpu || echo 2) 2>/dev/null | head -1 )
MAKE=make
command -v gmake >/dev/null 2>&1 && MAKE=gmake

# FORCE_UNSAFE_CONFIGURE: coreutils configure balks at uid 0 (some CI
# VMs); the golden harness separately refuses root, so this is inert
# there and harmless here.
build_oracle() {
    (
        cd "$outabs"
        [ -x config.status ] || FORCE_UNSAFE_CONFIGURE=1 "$srcabs/configure" \
            --quiet --disable-nls --without-selinux --disable-acl \
            ${CC:+CC="$CC"} >configure.log 2>&1
        # `make src/cp` shortcut has raced under high -j on fresh trees;
        # full-make fallback, then retry the target (tally's ladder).
        "$MAKE" -s -j"$jobs" src/cp >make.log 2>&1 \
            || "$MAKE" -s -j"$jobs" >>make.log 2>&1 \
            || "$MAKE" -s -j"$jobs" src/cp >>make.log 2>&1
    )
}
# A cached build dir can hold config.status from an older OS image whose
# gnulib decisions no longer match the libc; one clean-slate retry heals
# that class (tally, 2026-07-16).
build_oracle || {
    echo "build-gnu-cp: build failed; retrying from a clean build dir" >&2
    rm -rf "$outabs"
    mkdir -p "$outabs"
    build_oracle || {
        echo "build-gnu-cp: coreutils build failed; log tails follow" >&2
        tail -5 "$out/configure.log" >&2 2>/dev/null || true
        tail -30 "$out/make.log" >&2 2>/dev/null || true
        exit 1
    }
}

"$bin" --version | sed -n 1p | grep -q " $pin\$" || {
    echo "build-gnu-cp: built cp does not report version $pin; refusing" >&2
    exit 1
}

# Feature pinning: record and assert. USE_XATTR=1 requires the attr dev
# package on the build host (ubuntu: libattr1-dev, alpine: attr-dev,
# arch: attr).
features=$(grep -E '^#define (USE_XATTR|USE_ACL|HAVE_SELINUX_SELINUX_H|ENABLE_NLS)( |$)' \
    "$out/lib/config.h" 2>/dev/null || true)
printf '%s\n' "$features" > "$out/oracle-features.txt"
printf '%s\n' "$features" | grep -q '#define USE_XATTR 1' || {
    echo "build-gnu-cp: oracle built WITHOUT xattr support; refusing." >&2
    echo "  install the attr dev package and rerun (rm -rf $out first)" >&2
    cat "$out/oracle-features.txt" >&2
    exit 1
}
if printf '%s\n' "$features" | grep -q '#define USE_ACL 1'; then
    echo "build-gnu-cp: oracle built WITH ACL support despite --disable-acl; refusing" >&2
    exit 1
fi
if printf '%s\n' "$features" | grep -q '#define HAVE_SELINUX_SELINUX_H 1'; then
    echo "build-gnu-cp: oracle built WITH SELinux support; refusing" >&2
    exit 1
fi

printf '%s\n' "$bin"
