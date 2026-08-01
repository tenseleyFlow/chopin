#!/bin/sh
# ci/release.sh - the v0.1.0 release ritual (sprint 11C).
#
# Ported from tally/ci/release.sh with its two hard-won rules:
#   - gate on the bench/gate EXIT CODE, never a grep for "FAIL"
#     (a grep overmatched an informational row and aborted a release);
#   - packaging sha comes from the PUBLISHED asset, never a local
#     rebuild (tarballs differ by mtime/ordering).
#
# Phases: verify -> dist -> clean-room -> [STOP] -> tag+publish.
# Everything before the STOP is local and repeatable. Tagging and
# publishing are outward-facing and irreversible, so they run only
# with --publish, which a human passes deliberately.

set -eu
cd "$(dirname "$0")/.."

VER=$(sed -n 's/^CHOPIN_VERSION="\(.*\)"/\1/p' configure)
TAG="v$VER"
PUBLISH=0
[ "${1:-}" = "--publish" ] && PUBLISH=1

MAKE=make
command -v gmake >/dev/null 2>&1 && MAKE=gmake

echo "== chopin $TAG =="

[ -z "$(git status --porcelain)" ] || {
    echo "release: working tree not clean" >&2; exit 1; }
if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null 2>&1; then
    echo "release: tag $TAG already exists" >&2; exit 1
fi
case $VER in
0.0.*) echo "release: version still $VER (bump configure)" >&2; exit 1 ;;
esac

echo "== verify: build =="
$MAKE clean >/dev/null
$MAKE

echo "== verify: docs generated from the register =="
$MAKE man-check

echo "== verify: correctness suites =="
$MAKE check

echo "== verify: release-depth fuzz + identity =="
$MAKE fuzz
$MAKE identity

echo "== verify: sanitizers =="
$MAKE sanitize
$MAKE tsan

echo "== verify: perf gates (exit code is the gate) =="
gaterc=0
sh bench/gates.sh > "build/gates-$VER.txt" 2>&1 || gaterc=$?
cat "build/gates-$VER.txt"
[ "$gaterc" = 0 ] || { echo "release: perf gates not clean" >&2; exit 1; }

budgetrc=0
sh bench/syscall-budget.sh > "build/budget-$VER.txt" 2>&1 || budgetrc=$?
cat "build/budget-$VER.txt"
[ "$budgetrc" = 0 ] || {
    echo "release: syscall budget not clean" >&2; exit 1; }

echo "== verify: release matrices present =="
for m in bench/release/matrix-v$VER-*.txt; do
    [ -r "$m" ] || {
        echo "release: no bench/release matrix for $VER" >&2; exit 1; }
    echo "  $m"
done

echo "== verify: deviation register dispositioned =="
sh scripts/check-register.sh

echo "== dist =="
$MAKE dist
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "chopin-$VER.tar.gz" > "chopin-$VER.tar.gz.sha256"
else
    shasum -a 256 "chopin-$VER.tar.gz" > "chopin-$VER.tar.gz.sha256"
fi

echo "== clean-room build from the tarball =="
CR=$(mktemp -d)
tar -xzf "chopin-$VER.tar.gz" -C "$CR"
(
    cd "$CR/chopin-$VER"
    ./configure >/dev/null
    $MAKE >/dev/null
    ./chopin --version | grep -q "$VER"
    printf 'clean-room\n' > s
    ./chopin s d && [ "$(cat d)" = "clean-room" ]
)
rm -rf "$CR"
echo "clean-room: build + copy verified"

if [ "$PUBLISH" != 1 ]; then
    cat <<EOF

== STOP: everything local is verified ==
Nothing has been tagged, pushed, or published.

To publish (irreversible, outward-facing):
    sh ci/release.sh --publish

That will: tag $TAG, push the tag, and create the GitHub release with
the tarball + sha + matrices. Afterwards, pin packaging/PKGBUILD and
packaging/chopin.rb to the sha of the PUBLISHED asset (not the local
tarball) and push the AUR + tap updates.
EOF
    exit 0
fi

echo "== tag + GitHub release =="
git tag -a "$TAG" -m "chopin $TAG"
git push origin "$TAG"
gh release create "$TAG" \
    --title "chopin $TAG" \
    --notes-file doc/RELEASE-NOTES-$VER.md \
    "chopin-$VER.tar.gz" "chopin-$VER.tar.gz.sha256" \
    bench/release/matrix-v$VER-*.txt

echo "== published. Pin packaging to the PUBLISHED asset sha =="
rm -f "published-$VER.tar.gz"
gh release download "$TAG" --pattern "chopin-$VER.tar.gz" \
    --output "published-$VER.tar.gz"
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "published-$VER.tar.gz"
else
    shasum -a 256 "published-$VER.tar.gz"
fi
echo "Put that sha in packaging/PKGBUILD and packaging/chopin.rb."
