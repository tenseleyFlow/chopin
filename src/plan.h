#ifndef CHOPIN_PLAN_H
#define CHOPIN_PLAN_H

#include <stdbool.h>

#include "options.h"

/* The decision module (overview s4): per-invocation, which stat calls
   are needed, which data engines are eligible, whether parallelism
   engages. Sprint 01 ships the skeleton: everything routes scalar,
   parallel never engages, the FICLONE cache exists as a flag only.
   Engines land in sprint 07, the pool in 09, single-stat planning is
   sprint 10's measured decision. */

struct chopin_plan {
    bool dst_stat_needed;       /* false for 2-arg nonexistent dst */
    bool reflink_eligible;      /* FICLONE ladder rung (sprint 07) */
    bool offload_eligible;      /* copy_file_range rung (sprint 07) */
    bool sparse_eligible;       /* sparse engines rung (sprint 07) */
    bool parallel_eligible;     /* always false until sprint 09 */
    bool ficlone_cache_enabled; /* per-(src_dev,dst_dev) memo, sprint 07 */
};

void chopin_plan_init(struct chopin_plan *plan,
                      const struct chopin_options *x, bool new_dst);

/* CHOPIN_DEBUG_PLAN=1 prints the plan to stderr (never stdout: the
   byte-parity surface stays clean). */
void chopin_plan_maybe_debug(const struct chopin_plan *plan);

#endif
