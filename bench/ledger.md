# Performance ledger

Per-change measurements (sprint 10B) and lane baselines. Dev-scale
numbers are for iteration; release-scale tables ship with v0.1.0.
Machine "kasumi": x86-64 16c, btrfs on NVMe, kernel per
results/machine.txt. All numbers min-of-3, LC_ALL=C; cold = sync +
drop_caches before every run.

## Baseline (sprint 09 engine, pre-10B), dev scale, 2026-07-20

swarm 50k x 1K cold:
  chopin 1.004  chopin-serial 1.357  gnu 1.320
  fcp 0.636  xcp 0.649  wcp 1.233(tree-mismatch: no fidelity)
reflink 4k x 64K warm (btrfs FICLONE):
  chopin 0.062  chopin-serial 0.088  gnu 0.084
  fcp 0.055  xcp 0.057  wcp 0.195(tree-mismatch)
smallfile 1 x 4K warm (startup floor):
  chopin 0.000264  gnu 0.000283  fcp 0.000704  xcp 0.000598
  wcp 0.0106

Gates: startup-floor PASS (chopin < gnu); throttled-io PASS (par
1.02 vs ser 1.41 under 20MB/s emulated-rotational); ficlone-cache
PASS (1 probe/100 files); spine-walk PASS (65ms / 65k files, budget
200ms).

Syscall budget: minimal 2-arg chopin=88 gnu=92 total; per-file
marginal 10 == 10; FICLONE ioctls 1 vs GNU 33 on a 33-file non-CoW
copy.

Reading: chopin already beats GNU and locally-measured wcp on cold
swarm; fcp/xcp lead by ~1.55x there - the 10B traversal work
(getdents64, openat-relative, statx batching, inode ordering) is
aimed at exactly that gap. wcp's published 2.4x cold-swarm bar does
NOT reproduce on this machine against 9.11 (it loses to GNU) -
the bar-staleness note vindicated; the binding local bars are
fcp/xcp.

## 10B changes

All dev scale, kasumi, min-of-3 unless noted. Sequence is
cumulative; correctness re-proven after each step (goldens 150,
identity trials, differential fuzz; sanitize + TSan at the end).

1. handoff 64->256 + fstat 3->2/file (clone_file takes dst_dev from
   copy_reg's fstat) + advisory per-dir stat prefetch:
   swarm cold 1.004 -> 0.848.
2. SUBTREE prefetch cascade (workers stat one dir, self-submit child
   dirs; fcp's parallel traversal recast as cache warming - advisory,
   ordering-free): swarm cold 0.848 -> 0.739. Evidence: cold spine
   walk alone 1.23s vs warm 0.12s - the serial cold stats were the
   bottleneck, and the cascade absorbs them in parallel.
3. Scout-fed floors (the cascade's file/byte counts trip activation
   ahead of the spine): neutral here (noise band), principled for
   big-file trees.
4. Deferred no-op dir barriers (barrier only when the dir's
   post-order metadata mutates something) + signal-not-broadcast for
   small batches + 8192-slot drain cap:
   kernel-tree warm 1.26 -> 0.71 (fcp 0.87: the WARM CLASS BAR is
   BEATEN by 18%); kernel cold 0.82 vs fcp 0.92; swarm 0.74 -> 0.68.
   COST: exposed a real UAF (payloads borrowed copy_dir's stack
   options; frames now die pre-join) -> payloads own options BY
   VALUE. Caught by the identity harness (0-byte files), fixed,
   re-proven. The 09 assumption "copy_dir barriers before its frame
   dies" is retired.
5. GNU SAME_OWNER_AND_GROUP chown-skip ported (copy.c:1071 - a
   missed guard from the sprint 04 port; the oracle's own trace
   showed 0 fchowns where chopin paid 2065): hardlink-farm
   0.200 -> 0.186. Metadata-order unit pin updated (fchown now
   optional, position still pinned).
6. Wide-dir gate (>=32 entries) on the cascade: tiny -R copies no
   longer spawn the pool; smallfile restored to 0.236-0.261ms,
   FASTER than GNU (0.243-0.275ms).
7. geteuid cached (2000 calls -> 1); xattr value buffer moved off
   a shared static (latent worker race found by inspection).
8. memcpy path join in copy_dir: swarm-empty 2.32 -> 2.21 = GNU
   parity (2.20).

## Dispositions (measured)

- CHUNKED default: OFF. large-nocow warm chunked 0.615 vs plain
  0.578; cold equal. No win on btrfs/NVMe; revisit only with ext4/
  nomad evidence.
- O_DIRECT source reads: NOT LANDED. Cold 1G read: buffered 0.628s,
  O_DIRECT 0.819s on this NVMe (readahead wins). Measured basis.
- Single-stat 2-arg variant: DROPPED per the <2% complexity floor.
  The eliminated statx is <1% of a 240us invocation (B1: chopin
  already 88 <= gnu 92 total).

## Standing results after 10B (dev scale)

swarm cold:        chopin 0.68  gnu 1.45  fcp 0.64  xcp 0.65
                   (class bar wcp 1.23 fidelity-failing: BEATEN 1.8x;
                   fcp leads by 6% - report line)
kernel-tree warm:  chopin 0.71  gnu 1.23  fcp 0.87  (CLASS BAR BEATEN)
kernel-tree cold:  chopin 0.82  gnu 1.70  fcp 0.92
large-nocow warm:  chopin 0.578 gnu 0.660; cold 0.70 vs 0.99
reflink 4k x 64K:  chopin 0.051 gnu 0.066  fcp 0.043  xcp 0.043
smallfile:         chopin 0.236ms gnu 0.243ms (floor gate PASS)
swarm-empty:       chopin 2.21  gnu 2.20 (parity; cpz bar
                   report-not-gate, not installable)
sparse-nocow:      chopin 0.0227 gnu 0.0216 (1.05x - watch)
hardlink-farm:     chopin 0.186 gnu 0.153 (1.2x - OPEN ITEM;
                   syscall counts now equal, gap is sub-syscall;
                   openat-relative traversal remains the candidate)
Gates: ALL PASS (startup, throttled-io par 0.71 vs ser 1.50,
ficlone-cache 1 probe, spine-walk 59ms/65k).
Budget: B1 88<=92, B2 9<=10/file, B3 1 ioctl vs 33.

## RELEASE-SCALE dev-box table (kasumi, 2026-07-20, min-of-3)

Locked lane sizes: swarm 200k x 1K, empty 1M, kernel 65k, large
4 GiB, sparse 2 GiB, hardlink 5k x 8, meta 20k xattrs, reflink 10k.

swarm cold:        chopin 2.84   ser 5.51   gnu 5.35   fcp 2.11
                   xcp 2.54   wcp 4.80(no-fidelity)
                   -> 1.88x GNU; wcp CLASS BAR beaten 1.7x; fcp
                   leads (report line)
swarm-empty 1M:    chopin 11.55  ser 11.38  gnu 10.98  fcp 6.24
                   xcp 12.88  wcp 10.44(no-fidelity)
                   -> 1.03-1.05x of GNU, tolerance tie; cpz bar
                   report-not-gate; empties now forced serial
kernel warm:       chopin 0.689  gnu 1.216  fcp 0.819  xcp 0.743
                   wcp 2.59(no-fidelity)
                   -> CLASS BAR (fcp warm) beaten 16%; beats xcp too
kernel cold:       chopin 0.783  gnu 1.683  fcp 0.886  xcp 1.013
large reflink:     chopin 0.69ms fastest everywhere (gnu 0.79ms,
                   wcp real-copies 2.65s)
large-nocow 4G:    warm chopin 2.580 ~ gnu 2.584; cold 2.72 vs 2.88
sparse reflink:    chopin 0.57ms ~ gnu 0.54ms
sparse-nocow:      chopin 0.1836 ~ gnu 0.1824 (1.006 tie)
hardlink-farm:     chopin 0.506  gnu 0.400  (1.26x - OPEN ITEM)
metadata-heavy:    chopin 0.517 ~ gnu 0.511 (fcp 0.318 no-fidelity)
reflink 10k:       chopin 0.117  gnu 0.184  fcp 0.121  xcp 0.119
                   -> fastest, cache+parallel
smallfile:         chopin 0.254ms  gnu 0.259ms -> fastest

Victory-lane verdicts at release scale: swarm (bar wcp) WON;
kernel (bar fcp warm) WON; large (bar xcp cold: xcp 0.0077 clone -
chopin 0.0032 clone, and nocow cold 2.72 vs xcp n/a) WON on both
readings; reflink WON; smallfile WON. Ties within guard tolerance:
swarm-empty, sparse, metadata, large-nocow-warm. LOSS: hardlink-farm
1.26x (no published bar; open item). fcp remains ahead on
swarm/empty flat-tree lanes - report lines, not class bars.

## NOMAD-1 warm table (M5 Pro, APFS, pinned 9.11 oracle built
## on-box, dev scale, 2026-07-20; cold is Linux-only)

Pre-clone-engine highlights (first run) and post-engine reruns:

swarm-empty:    chopin 10.12  ser 9.96   gnu 19.19  fcp 14.40
                xcp 7.20   -> 1.9x GNU
kernel warm:    chopin 4.92   ser 12.70  gnu 6.94   fcp 4.66
                xcp 5.62   -> 1.41x GNU; fcp 5% ahead (report)
large-nocow:    chopin 0.163  gnu 0.313  -> 1.9x GNU
sparse-nocow:   chopin 0.0219 gnu 0.0514 -> 2.3x GNU
hardlink-farm:  chopin 2.298  gnu 2.329  -> WIN (the Linux 1.26x
                loss does NOT reproduce on APFS)
metadata-heavy: chopin 1.028  gnu 1.211  -> 1.18x GNU
reflink 4k:     chopin 0.226  gnu 0.363  fcp 0.210  xcp 0.412
smallfile:      chopin 1.40ms gnu 1.50ms -> fastest

APFS CLONE ENGINE (sprint 07B stub completed here - the first mac
machine with a pinned oracle):
large-single:   0.166s -> 1.41ms (117x; gnu 1.25ms - clone latency)
sparse:         44ms   -> 1.49ms (gnu 1.34ms)
The 0.16ms residual vs GNU at clone scale is probe/startup
overhead; the lane's class bar (xcp: 148ms here) is beaten 100x.

Mac parity: golden 148/148 + 1 build-parity skip (--preserve=xattr
FATALS in any macOS coreutils build - libattr does not exist there;
chopin's Darwin xattr backend succeeds and is KEPT as a deliberate
capability carve-out, DEV-006 family). Two mac parity fixes landed:
scantype inference under --sparse=never follows GNU's no-cfr shape
(word "no", not "SEEK_HOLE"), and the golden xattr case probes
oracle capability. Mac identity: 30/30 byte-exact.

## Open-item closure: hardlink-farm (2026-07-20)

Profiling convicted chopin_src_to_dest_lookup: a LINEAR SCAN at
3.07% of cycles (6x GNU's entire gnulib-hash cost) - O(groups x
links) on this lane. Replaced with open addressing keyed on
(dev,ino) (tombstone deletion; src_info/dest_info keep linear scans
- they hold command-line operands only). Recovered ~6ms/dev,
~30ms/release.

The residual 1.15x is TRAVERSAL-ORDER LOCALITY, proven by strace -T:
identical linkat counts and identical argument shapes, but 11.7us vs
9.7us per call - GNU's inode-order traversal links group siblings
adjacently while the inode's refs are journal-hot; chopin's
deterministic name-order returns to each inode thousands of files
later. Reordering serial processing would break the parallel==serial
byte-identity contract for a lane with no published bar that does
not reproduce on APFS (nomad: chopin WINS it). ACCEPTED as a
measured cost of the determinism pillar (overview s2); recorded
here, not gated.

Post-fix: dev 0.180 vs gnu 0.158 (1.14x); release 0.474 vs 0.404
(1.17x); nomad 2.298 vs 2.329 (win).

## METHODOLOGY CORRECTION (sprint 11A, 2026-08-01)

Every number ABOVE this line was produced by bench/run-all.sh, which
drives hyperfine. Three flaws were found while assembling the release
tables; the numbers above should be read as +/-10% and the corrected
A/B table below supersedes them where they disagree.

1. ORDERING BIAS. hyperfine runs all reps of tool A, then all of B.
   Whichever runs second reads a source the first just pulled into
   page cache. Worth ~10% on warm bulk lanes. It manufactured a
   large-single "regression" (0.86x) that an interleaved run shows as
   1.06x, and it under-reported chopin against fcp/xcp (matrix said
   fcp led swarm-warm; balanced says chopin leads 1.08x).
2. TIMER RESOLUTION. /usr/bin/time -f %e reports 10 ms - 5% of a
   0.2 s lane. The first A/B pass used it and read metadata-heavy as
   0.88x; nanosecond timing reads 0.94-0.97x.
3. RIVAL FLAGS. ab.sh passed cp's flags to rivals; fcp rejects -R,
   exited in 1 ms, and scored a 500x "win" because a nonzero exit was
   timed rather than failed. Nonzero exits are now fatal.

bench/ab.sh is the corrected instrument: interleaved ABBA ordering,
nanosecond clocks, per-tool flag translation, fatal nonzero exits.
run-all.sh remains useful for broad sweeps and result-tree
verification, not for close calls.

## v0.1.0 TABLE (kasumi, corrected instrument)

vs pinned GNU 9.11, min of 6 interleaved reps (release scale where
noted):
  kernel-tree cold  2.12x     swarm cold        2.09x
  kernel-tree warm  1.71x     reflink           1.58x
  swarm warm        1.55x     large-nocow 4G    1.06x
  sparse-nocow      1.03x     smallfile         1.00x
  swarm-empty       0.96x warm / 0.99x release  SLOWER
  metadata-heavy    0.94x warm / 0.97x release  SLOWER
  hardlink-farm     0.84x warm / 0.81x release  SLOWER

vs locally re-baselined rivals (same fixtures, same instrument):
  vs fcp:  kernel 1.22x, swarm warm 1.08x, reflink 1.04x,
           swarm cold 0.91x, swarm-empty 0.70x
  vs xcp:  swarm-empty 1.14x, kernel 1.07x, reflink 1.06x,
           swarm warm 1.01x, swarm cold 1.01x

Reading: chopin wins where bytes move or a tree is walked; it is at
parity or a few percent behind on pure-metadata trees, where the pool
has no payload to overlap and name-order traversal costs the inode
locality GNU gets from inode order. hardlink-farm is the one durable
loss and is a deliberate trade (determinism over locality).

## Nomad status

The Apple-silicon table above predates the correction and predates
the release-scale rerun (the box dropped off the tailnet mid-run;
ssh SIGHUP killed the matrix - bench/run-nomad.sh now launches
detached with nohup so a dropped tunnel cannot kill a run). The
large-margin mac results (APFS clone 166ms -> 1.4ms; 1.9-2.3x GNU on
several lanes) are far outside the +/-10% bias band and stand. The
near-parity mac rows (e.g. hardlink 2.298 vs 2.329) do not, and are
not quoted in the README.

## NOMAD RE-MEASUREMENT (corrected instrument, 2026-08-01)

Supersedes the sprint-10 nomad table above, which was collected with
the sequential instrument AND before the APFS clone engine existed.

  sparse-nocow 2.14x   reflink 1.69x   swarm 1.53x   kernel 1.42x
  swarm-empty 1.01x    large-nocow 0.98x   hardlink-farm 0.98x
  metadata-heavy 0.97x  smallfile 0.95x

Two sprint-10 nomad claims are WITHDRAWN:

1. "hardlink-farm: chopin 2.298 vs gnu 2.329 -> WIN; the Linux loss
   does NOT reproduce on APFS." Corrected: 2.335 vs 2.287 = 0.98x,
   chopin slightly SLOWER. The loss does reproduce on APFS - it is
   just far milder (2% vs 19%), which fits the locality explanation:
   APFS does not punish revisiting an inode late as hard as btrfs.
2. "swarm-empty 1.9x GNU." Corrected: 1.01x. The old gap was an
   ARTIFACT OF AN UNIMPLEMENTED FEATURE: GNU clones every dest on
   APFS via fclonefileat, chopin did not yet, and not-cloning is
   cheaper than cloning. Implementing the clone engine faithfully
   (sprint 10C) moved the lane to parity. That is parity working as
   intended, not a regression.

Investigated and rejected: skip fclonefileat for zero-length sources
(would restore ~1.9x). GNU reports "reflink: yes" for cloned empty
files under --debug, which is byte-parity surface; the speed is not
available without diverging there. Parity wins ties (overview s3).

Also measured: the empty-file serial guard is NEUTRAL, not a win -
interleaved guard-vs-noguard is 0.99x on Linux and 1.02x on APFS.
Its original 1.5% justification was instrument noise. Kept on
principle (no payload, no pool dispatch), comment corrected.
