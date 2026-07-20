# chopin deviation register

Chopin is behavior-faithful to GNU coreutils cp 9.11 except where this
register applies. Policy (overview s3): QUIRKS are matched exactly;
genuine BUGS are fixed, each with a register entry here, a pinned
dev-tier case (tests/golden/dev/), a fuzz-filter line keyed to the ID
(tests/golden/DEVIATIONS), and an upstream report (filed, or a
signed-off draft - .docs/audits/upstream-reports.md holds drafts).
This file is tracked and ships: the chopin.1 DEVIATIONS section and
the README's fixed-bugs section are generated from it.

Classes: FIX (GNU bug, chopin corrects), QUIRK-KEPT (investigated,
matched anyway), BUILD-PARITY (not a deviation: matches GNU built
with the pinned feature set).

## DEV-001 double-space backup diagnostic [FIX - lands sprint 03]

GNU: `backing up %s might destroy source;  %s not copied` with two
spaces after the semicolon (copy.c:1934-1935) - a malformed message,
quirk register 2. chopin prints a single space.
Upstream: draft ready; filing recorded here when done.
Pinned by: dev-tier case (sprint 03).

## DEV-002 unquoted symlink-chown failure operand [FIX - lands sprint 05]

GNU: `failed to preserve ownership for %s` prints the destination
UNQUOTED in the symlink-ownership failure path (copy.c:2579-2580),
inconsistent with every neighboring diagnostic (quirk register 3).
chopin quotes it.
Upstream: draft ready.
Pinned by: dev-tier case (sprint 05).

## DEV-003 --update=older identical-symlink success [QUIRK-KEPT v0.1]

GNU treats a failed symlink creation as success when the existing
dest is an identical symlink under --update=older (copy.c:2541-2557);
GNU's own comment says the behavior "isn't documented, and seems
wrong". Kept for v0.1: scripts may depend on it; revisit with
upstream discussion. Matched exactly (sprint 05).

## DEV-004 same_nameat abrupt exit [FIX - lands sprint 03]

GNU: same_nameat calls error(1,...) mid-comparison when a parent
stat fails (lib/same.c:101,139), aborting the whole run instead of
failing the one file (quirk register 21). chopin reports
`cannot stat %s` and fails only that file.
Upstream: draft ready.
Pinned by: unit (samefile_driver dev004-parent-stat) - the failing
parent stat needs a mid-comparison race to reach through the CLI, so
the pin exercises the function surface directly.

## DEV-005 sparse_copy hole-punch success-on-failure [FIX - lands sprint 07]

GNU: sparse_copy returns success (0) when create_hole fails mid-file
(copy-file-data.c:237-238) - cp prints an error yet exits 0 with a
TRUNCATED destination; in lseek_copy the 0 is misread as "input file
shrank" (433-437). Data-loss class (quirk register 34). chopin
propagates the failure: nonzero exit, copy_reg failure envelope.
Upstream: draft ready.
Pinned by: dev-tier case (sprint 07).

## DEV-006 build-configuration parity notes [BUILD-PARITY]

Not deviations - chopin matches GNU cp built with the pinned oracle
feature set (scripts/build-gnu-cp.sh): no SELinux (-Z warn/ignore
paths), no POSIX-ACL support (mode bits only; ACL xattrs travel via
the xattr pass), xattr support ON with /etc/xattr.conf exclusions
not read (golden fixtures use user.* names only; sprint 04).
