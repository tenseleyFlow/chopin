# chopin 0.1.0

First release. chopin is a from-scratch C11 reimplementation of GNU
`cp`, behavior-faithful to coreutils 9.11.

## What it is

The complete GNU cp 9.11 option surface, with the same result trees,
the same diagnostics byte-for-byte, and the same exit status. `cpn`
is the same binary under a shorter name.

Parity is not asserted, it is tested. Every release runs:

- 150 golden cases compared against a feature-pinned coreutils 9.11
  build (result-tree manifest + stdout + stderr + exit status);
- 23 of GNU's own `tests/cp` scripts, unmodified, with all 66
  dispositioned in a tracked table;
- 600 differential fuzz trials across seeds, including hostile
  filenames and pre-existing destination states;
- 600 identity trials proving a parallel copy is byte-identical to a
  serial one;
- ASan/UBSan and ThreadSanitizer over the whole suite.

## Fixed GNU bugs

chopin fixes genuine defects rather than reproducing them. Each has a
register entry, a pinned regression test, and an upstream report.
See `doc/deviations.md` and the DEVIATIONS section of `chopin(1)`.

- **DEV-005** (data loss): GNU's `sparse_copy` returns success when a
  mid-file hole punch fails, so `cp` prints an error and exits 0 with
  a truncated destination.
- **DEV-004**: `same_nameat` aborts the entire run when a parent stat
  fails, instead of failing the one file.
- **DEV-001**, **DEV-002**: malformed diagnostics (a doubled space; an
  unquoted operand).

One GNU quirk was investigated and deliberately kept (DEV-003).

## Performance

Faster than GNU cp on every benchmarked workload except one, which is
named and explained in the README rather than omitted. Full tables,
methodology, and per-change measurements: README and `bench/ledger.md`.

## Requirements

libc and a C11 compiler. No external dependencies. GNU make (`gmake`
on BSD). Linux, macOS, and FreeBSD are tested in CI; Alpine (musl)
runs as a non-root user.
