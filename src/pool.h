#ifndef CHOPIN_POOL_H
#define CHOPIN_POOL_H

#include <stddef.h>

/* Worker pool (sprint 09A) - thread.c lineage from liszt/rank,
   reshaped from run-N-tasks-and-join to a submit/drain queue because
   cp's traversal streams payloads (liszt knew task_count up front).
   Handoff is BATCHED: one lock + one broadcast per submitted batch,
   never per-file signaling (the swarm lane dies by signaling
   overhead).

   Lifetime is a measured decision (09A bench): PERSISTENT parks
   workers on the condvar between batches; EPHEMERAL spawns at submit
   and joins at drain, paying create/join per batch like liszt's
   per-directory pools. CHOPIN_POOL_SHAPE=ephemeral selects the
   latter; the default is the bench winner.

   Sizing: CHOPIN_PARALLEL_WORKERS overrides (0 disables); default is
   the logical core count - Darwin uses hw.perflevel0.logicalcpu so
   efficiency cores do not dilute the pool - capped at 16 and clamped
   so worker fds (2 each) plus spine headroom stay under
   RLIMIT_NOFILE. */

typedef void (*chopin_pool_fn)(void *arg);

/* Resolved worker count; 0 = pool disabled (serial). Stable across a
   run; safe to call before the pool exists. */
int chopin_pool_workers(void);

/* Enqueue a batch. Starts workers lazily on first use. The args stay
   owned by the caller until chopin_pool_drain returns. */
void chopin_pool_submit(chopin_pool_fn fn, void *const *args, size_t n);

/* Enqueue at the FRONT of the queue - for advisory work (cache
   prefetch) that is only useful if it runs before the copy payloads
   already queued behind the spine's current position. */
void chopin_pool_submit_front(chopin_pool_fn fn, void *const *args,
                              size_t n);

/* Block until every submitted payload has executed. Ephemeral shape
   also joins the workers. */
void chopin_pool_drain(void);

/* Join and free everything (persistent shape); harmless if the pool
   never started. Call once, after the final drain. */
void chopin_pool_shutdown(void);

#endif
