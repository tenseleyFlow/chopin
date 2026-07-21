#include "plan.h"

#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "parallel.h"
#include "pool.h"

void
chopin_plan_init(struct chopin_plan *plan, const struct chopin_options *x,
                 bool new_dst)
{
    /* GNU already elides the dst stat for a 2-arg nonexistent dst
       (copy.c:1747-1748); the plan records it for the engine sprints. */
    plan->dst_stat_needed = !new_dst;

    const char *fs = getenv("CHOPIN_FORCE_SCALAR");
    bool force_scalar = fs != NULL && *fs != '\0' && *fs != '0';

    plan->reflink_eligible = !force_scalar
        && (CHOPIN_HAVE_FICLONE || CHOPIN_HAVE_FCLONEFILEAT)
        && x->reflink_mode != CHOPIN_REFLINK_NEVER
        && x->data_copy_required;
    plan->offload_eligible = !force_scalar
        && CHOPIN_HAVE_COPY_FILE_RANGE
        && x->data_copy_required
        && x->sparse_mode != CHOPIN_SPARSE_ALWAYS;
    plan->sparse_eligible = x->data_copy_required
        && x->sparse_mode != CHOPIN_SPARSE_NEVER;
    plan->parallel_eligible = chopin_pool_workers() > 0
        && !x->debug && x->data_copy_required;
    plan->ficlone_cache_enabled = plan->reflink_eligible
        && x->reflink_mode != CHOPIN_REFLINK_ALWAYS;
}

void
chopin_plan_maybe_debug(const struct chopin_plan *plan)
{
    const char *e = getenv("CHOPIN_DEBUG_PLAN");

    if (e == NULL || *e == '\0' || *e == '0')
        return;
    fprintf(stderr,
            "chopin plan: dst_stat=%d reflink=%d offload=%d "
            "sparse=%d parallel=%d workers=%d ficlone_cache=%d\n",
            plan->dst_stat_needed, plan->reflink_eligible,
            plan->offload_eligible, plan->sparse_eligible,
            plan->parallel_eligible, chopin_pool_workers(),
            plan->ficlone_cache_enabled);
}
