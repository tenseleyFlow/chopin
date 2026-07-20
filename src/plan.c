#include "plan.h"

#include <stdio.h>
#include <stdlib.h>

void
chopin_plan_init(struct chopin_plan *plan, const struct chopin_options *x,
                 bool new_dst)
{
    /* GNU already elides the dst stat for a 2-arg nonexistent dst
       (copy.c:1747-1748); the plan records it for the engine sprints. */
    plan->dst_stat_needed = !new_dst;

    /* Scalar-only era: the ladder parses (sprint 01) but every rung
       routes scalar until sprint 07; flags stay honest. */
    plan->reflink_eligible = false;
    plan->offload_eligible = false;
    plan->sparse_eligible = false;
    plan->parallel_eligible = false;
    plan->ficlone_cache_enabled = false;

    (void)x;
}

void
chopin_plan_maybe_debug(const struct chopin_plan *plan)
{
    const char *e = getenv("CHOPIN_DEBUG_PLAN");

    if (e == NULL || *e == '\0' || *e == '0')
        return;
    fprintf(stderr,
            "chopin plan: engine=scalar dst_stat=%d reflink=%d offload=%d "
            "sparse=%d parallel=%d ficlone_cache=%d\n",
            plan->dst_stat_needed, plan->reflink_eligible,
            plan->offload_eligible, plan->sparse_eligible,
            plan->parallel_eligible, plan->ficlone_cache_enabled);
}
