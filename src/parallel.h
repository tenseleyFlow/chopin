#ifndef CHOPIN_PARALLEL_H
#define CHOPIN_PARALLEL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Sprint 09B - the serial spine's view of the pool (overview s7).
   The spine owns traversal, hash tables, ALL output, and ordering;
   workers execute regular-file payloads only. stderr from both is
   sequenced through ordered slots and replayed in traversal order at
   barriers, so parallel output is byte-identical to serial output
   per stream. stdout (-v lines) never enters a slot: the spine emits
   it at decision time, which is traversal order by construction. */

/* True once the aggregate floors (CHOPIN_PARALLEL_MIN: file count
   AND bytes; default 4 files / 8 MiB) have been crossed and the pool
   has workers. note_eligible feeds the accounting. */
bool chopin_parallel_active(void);
void chopin_parallel_note_eligible(off_t bytes);

/* Device-pair classing: rotational or network pairs stay serial
   (same-device network pairs stay eligible - server-side offload).
   Memoized per src device; the dst side is classed once from the
   target dirfd. */
void chopin_parallel_class_dst(int dst_dirfd);
bool chopin_parallel_pair_ok(dev_t src_dev, const char *src_name);

/* Dispatch: run(payload) executes on a worker with diagnostic
   capture installed; join(payload, ok) runs on the spine at replay
   time, in traversal order. dst_path is registered in the
   pending-path set until the next barrier. */
void chopin_parallel_dispatch(bool (*run)(void *),
                              void (*join)(void *, bool),
                              void *payload, const char *dst_path);

/* True if the path names a dest whose payload has not yet joined. */
bool chopin_parallel_pending_path(const char *path);

/* Advisory cold-metadata subtree prefetch (sprint 10B): a worker
   cascade stats every entry under dir to warm the dentry/inode
   cache ahead of the spine's serial ladder stats. Fire-and-forget;
   call once per command-line directory operand. */
void chopin_parallel_prefetch_tree(const char *dir);

/* Drain every in-flight payload, replay all slot stderr in traversal
   order, run join hooks, clear the batch. Returns the AND of payload
   results (true when nothing was pending). */
bool chopin_parallel_barrier(void);

/* Drain + replay, preserving any payload failure for the next
   barrier's return instead of consuming it here. Used mid-batch:
   before a prompt, and by the pending-path entry guard. */
void chopin_parallel_drain_keep(void);

/* Interactive prompt incoming: drain + replay so the prompt appears
   after all earlier stderr, then leave capture off. */
void chopin_parallel_flush_for_prompt(void);

/* Final teardown (joins pool workers). */
void chopin_parallel_shutdown(void);

#endif
