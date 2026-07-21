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
