# chopin

A from-scratch C11 reimplementation of GNU `cp`, behavior-faithful to
coreutils 9.11.

Same options, same result trees, same diagnostics byte-for-byte, same
exit status — except for a short register of GNU bugs that chopin
fixes instead of reproducing. `cpn` is the same binary under a shorter
name.

```
cc, libc, GNU make. No other dependencies.

./configure && make && sudo make install
```

## Parity is tested, not claimed

The oracle is a feature-pinned build of coreutils 9.11, built from
source by `scripts/build-gnu-cp.sh`. Every comparison runs both tools
under `env -i` with the locale, timezone, and umask pinned, then
compares a manifest of the entire result tree (type, mode, owner,
size, content hash, link targets, hardlink groups, xattrs) plus
stdout, stderr, and the exit status.

| Tier | What it does |
| --- | --- |
| golden | 150 hand-built cases across the whole option surface |
| gnu-mined | 23 of GNU's own `tests/cp` scripts, run unmodified; all 66 dispositioned in `tests/gnu-mined/DISPOSITION` |
| fuzz | 600 differential trials: random flag draws, hostile filenames, pre-existing destination states |
| identity | 600 trials proving parallel output is byte-identical to serial |
| sanitizers | ASan/UBSan and ThreadSanitizer over the full suite |

`CHOPIN_DEBUG_VERIFY=1` re-reads every destination after copying and
compares it against the source. It is enabled throughout the suites,
so no fast path ships without the scalar loop having checked it.

CI runs Ubuntu, macOS, Alpine (musl, as a non-root user so permission
cases actually execute), FreeBSD, and a filesystems job with loopback
btrfs, XFS-reflink, and a size-bounded ext4 mount.

## Fixed GNU bugs

The deviation register is a feature, not an asterisk. Genuine defects
in GNU cp are fixed; each fix carries a register entry, a pinned
regression test, and an upstream report. Quirks — odd behavior that is
nonetheless deliberate or depended upon — are matched exactly.

| ID | Class | What |
| --- | --- | --- |
| DEV-001 | fix | Doubled space in the backup-refusal diagnostic |
| DEV-002 | fix | Unquoted operand in the symlink-ownership failure |
| DEV-003 | quirk kept | `--update=older` treats an identical existing symlink as success (GNU's own comment doubts it; scripts may depend on it) |
| DEV-004 | fix | `same_nameat` aborts the whole run on a parent-stat failure instead of failing one file |
| DEV-005 | fix | **Data loss**: `sparse_copy` reports success when a mid-file hole punch fails — GNU prints an error, exits 0, and leaves a truncated destination |
| DEV-006 | build parity | Records the pinned oracle's feature set (no SELinux, no POSIX ACLs, xattr on) |

Full text: `doc/deviations.md`, also rendered into `chopin(1)`.

## Performance

chopin is faster than GNU cp where there is data to move or a tree to
walk, and is at parity or slightly behind on workloads that are pure
metadata. It is not faster on everything, and the exceptions are in
the table rather than in a footnote.

Measured on kasumi (x86-64, 16 cores, btrfs on NVMe) against a
feature-pinned coreutils 9.11 build. Each number is the minimum of 6
interleaved repetitions, the two tools alternating order every
repetition; `sh bench/ab.sh <lane>` reproduces any row. Ratios are
chopin-relative — 1.55x means chopin took about two thirds the time,
0.84x means it took about a fifth longer.

| lane | what it copies | vs GNU cp |
| --- | --- | --- |
| kernel-tree, cold | 65k files, source-tree size distribution | **2.12x** |
| swarm, cold | 200k × 1 KiB | **2.09x** |
| kernel-tree, warm | 65k files | **1.71x** |
| reflink | 10k × 64 KiB on btrfs (clone path) | **1.58x** |
| swarm, warm | 200k × 1 KiB | **1.55x** |
| large-single | 4 GiB, raw copy (`--reflink=never`) | 1.06x |
| sparse | 2 GiB VM image with holes | 1.03x |
| smallfile | one 4 KiB file (startup floor) | 1.00x |
| swarm-empty | 1M zero-byte files | **0.99x** |
| metadata-heavy | 20k files, xattrs, deep tree, `-a` | **0.97x** |
| hardlink-farm | 5k inodes × 8 links, `-a` | **0.81x** |

Cold rows drop the page cache before every run. Warm rows do not.

### Where chopin is slower

**hardlink-farm, 0.81x.** This is a real, reproducible loss, and it is
a consequence of a deliberate design choice. chopin traverses
directories in sorted name order so that a copy is reproducible across
runs and filesystems; GNU traverses in inode order. On a tree with
dense hardlink groups, inode order links each group's siblings
back-to-back while that inode's references are still hot in the
journal, while name order returns to the same inode thousands of files
later. `strace -T` shows identical `linkat` counts and identical
arguments, at 11.7 µs per call versus GNU's 9.7 µs — the gap is
entirely locality.

Fixing it would mean reordering work on the traversal spine, which is
what guarantees that parallel output is byte-identical to serial
output. That guarantee is worth more than this lane, so the loss
stands. It does not reproduce on APFS.

**metadata-heavy (0.97x) and swarm-empty (0.99x)** sit just inside the
noise floor but land on the slow side often enough to report. Both are
workloads with no bytes to copy: the worker pool has nothing to
overlap, so chopin pays its per-file bookkeeping without the data-path
win that normally repays it.

### Against other cp replacements

Published figures for these tools date to 2021 and were measured
against a pre-`copy_file_range` GNU cp, so they are not usable as
bars. Every number here is a local re-measurement, same fixture, same
methodology, same machine, using the tools' own flag conventions.

| lane | vs fcp | vs xcp |
| --- | --- | --- |
| kernel-tree, warm | **1.22x** | **1.07x** |
| swarm, warm | **1.08x** | 1.01x |
| reflink | **1.04x** | **1.06x** |
| swarm, cold | **0.91x** | 1.01x |
| swarm-empty | **0.70x** | **1.14x** |

fcp is meaningfully faster than chopin on flat trees of tiny or empty
files. It also does not preserve hardlink structure — its result tree
fails the manifest comparison on the hardlink lane — so the two are
not doing the same amount of work. wcp is excluded from this table for
the same reason: its result trees do not match the source, and a copy
that copied wrong is not a faster copy. For the record, wcp's
published 2.4x cold-swarm claim does not reproduce here at all; it
loses to modern GNU cp on this machine.

### A note on measuring

Three measurement bugs were found and fixed while producing this
table, each of which had produced a wrong number first:

- running every repetition of one tool before the other lets the
  second tool read a source the first just pulled into page cache
  (worth ~10% on warm bulk lanes, and it invented a large-file
  regression that does not exist);
- `/usr/bin/time` resolves to 10 ms, which is 5% of a 0.2 s lane;
- rivals were being handed cp's flags, which fcp rejects — it exited
  in a millisecond and scored a 500x "win" until nonzero exits were
  made fatal.

The methodology and every per-change measurement are in
`bench/ledger.md`.

### Apple silicon

Re-measured with the same instrument on an M5 Pro, APFS, against a
coreutils 9.11 built on that machine. Cold rows are Linux-only, since
dropping the page cache needs `/proc/sys/vm/drop_caches`.

| lane | vs GNU cp |
| --- | --- |
| sparse | **2.14x** |
| reflink (APFS clone) | **1.69x** |
| swarm | **1.53x** |
| kernel-tree | **1.42x** |
| swarm-empty | 1.01x |
| large-single, raw copy | 0.98x |
| hardlink-farm | 0.98x |
| metadata-heavy | 0.97x |
| smallfile | 0.95x |

The shape agrees with Linux — wins where bytes move, parity or just
behind on metadata-only work — with two platform differences worth
naming. Sparse copying is much stronger on APFS (2.14x versus 1.03x
on btrfs). And hardlink-farm costs only 2% here against 19% on
Linux, because APFS does not pay the same journal-locality penalty
for revisiting an inode late.

One earlier claim is withdrawn. Before the macOS clone engine
existed, chopin appeared to beat GNU by 1.9x on empty-file trees.
That gap was not an optimization: GNU clones every destination on
APFS via `fclonefileat`, chopin did not yet, and skipping the clone
is cheaper than doing it. Now that the clone path is implemented
faithfully, the lane sits at parity. Skipping the clone for
zero-length sources would restore the gap, but GNU reports
`reflink: yes` for those files under `--debug`, and that output is
part of the parity surface — so the speed is not available without
lying about what happened.

## How it goes faster

- **A device-aware worker pool.** Regular-file payloads run on
  workers; the traversal spine keeps ownership of ordering, the hash
  tables, and all output. Rotational and network device pairs are
  detected and copied serially, because fan-out on spinning rust
  seeks itself to death.
- **Output never reorders.** Every byte of stdout and stderr is
  emitted on the spine in traversal order, with worker diagnostics
  captured and replayed at barriers. This is what the identity suite
  proves: same bytes, serial or parallel, at any worker count.
- **A reflink probe cache.** GNU attempts `FICLONE` once per file; on
  a non-CoW filesystem that is one failed ioctl per file. chopin
  memoizes the failure per device pair — 1 ioctl per run instead of
  one per file (measured: 1 versus 33 on a 33-file copy).
- **Advisory metadata prefetch.** A worker cascade warms the dentry
  and inode caches ahead of the spine's cold stats. It is pure cache
  warming: it keeps no results and touches no decision, so it cannot
  affect output.
- **The platform's fastest transfer path**, in order: reflink clone
  (`FICLONE`, or `fclonefileat` on macOS), `copy_file_range` offload,
  `SEEK_HOLE` sparse copying, and a scalar read/write loop that is
  always available as a fallback and as the verification oracle.

## Determinism

Directories are traversed in sorted name order. GNU traverses in
inode order, which varies by filesystem and across runs. Name order
means a chopin copy is reproducible; it also costs a little locality
on hardlink-dense trees, which is exactly the tradeoff visible in the
table above.

## Environment variables

`CHOPIN_PARALLEL_WORKERS`, `CHOPIN_PARALLEL_MIN`,
`CHOPIN_PARALLEL_CHUNKS`, `CHOPIN_CHUNK_THRESHOLD`,
`CHOPIN_FORCE_SCALAR`, `CHOPIN_DEBUG_PLAN`, `CHOPIN_DEBUG_STATS`,
`CHOPIN_DEBUG_VERIFY`, `CHOPIN_DEBUG_WALK`. All are for debugging,
testing, and benchmarking; none change user-visible output. See
`chopin(1)` for what each does.

## Building and testing

```
./configure          # hand-rolled, no autotools
make
make check           # unit + golden + mined + fuzz smoke + identity smoke + perf smoke
make fuzz            # 3 seeds x 200 differential trials
make identity        # 600 parallel-vs-serial identity trials
make sanitize        # ASan/UBSan over the suite
make tsan            # ThreadSanitizer with the pool forced on
sh bench/run-all.sh  # the benchmark matrix
sh bench/gates.sh    # the machine-checkable performance gates
```

The parity suites need the pinned oracle:
`sh scripts/build-gnu-cp.sh`. Without it they skip rather than
pretend.

## License

GPL-3.0-or-later. chopin is not part of GNU coreutils and is not
endorsed by the GNU project; it reimplements the documented behavior
of GNU cp.
