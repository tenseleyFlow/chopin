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

(entries appended per change: before/after on the affected lanes)
