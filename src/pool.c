#include "pool.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "config.h"
#include "copydata.h"
#include "quote.h"
#include "util.h"

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

enum { POOL_MAX_WORKERS = 16 };

struct task {
    chopin_pool_fn fn;
    void *arg;
};

static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_avail = PTHREAD_COND_INITIALIZER;
static pthread_cond_t batch_done = PTHREAD_COND_INITIALIZER;

static struct task *queue;      /* ring buffer */
static size_t q_cap, q_head, q_len;
static size_t in_flight;        /* popped, still executing */
static bool shutting_down;

static pthread_t workers[POOL_MAX_WORKERS];
static int n_workers;           /* threads currently running */
static int resolved_workers = -1;
static bool ephemeral;

static int
darwin_perf_cores(void)
{
#if defined(__APPLE__)
    int n = 0;
    size_t len = sizeof n;

    if (sysctlbyname("hw.perflevel0.logicalcpu", &n, &len, NULL, 0) == 0
        && n > 0)
        return n;
#endif
    return 0;
}

int
chopin_pool_workers(void)
{
    if (resolved_workers >= 0)
        return resolved_workers;

    long n = 0;
    const char *e = getenv("CHOPIN_PARALLEL_WORKERS");

    if (e != NULL && *e != '\0') {
        n = strtol(e, NULL, 10);
        if (n < 0)
            n = 0;
    } else {
        n = darwin_perf_cores();
        if (n <= 0)
            n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 0)
            n = 1;
    }
    if (n > POOL_MAX_WORKERS)
        n = POOL_MAX_WORKERS;

    /* Workers hold two fds each; keep them plus spine headroom under
       the soft RLIMIT_NOFILE. */
    struct rlimit rl;

    if (getrlimit(RLIMIT_NOFILE, &rl) == 0
        && rl.rlim_cur != RLIM_INFINITY) {
        long budget = ((long)rl.rlim_cur - 64) / 2;

        if (budget < 0)
            budget = 0;
        if (n > budget)
            n = budget;
    }
    if (n == 1)
        n = 0;      /* one worker is just the spine with extra steps */

    e = getenv("CHOPIN_POOL_SHAPE");
    ephemeral = e != NULL && strcmp(e, "ephemeral") == 0;

    resolved_workers = (int)n;
    return resolved_workers;
}

static void *
worker_main(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&pool_lock);
    for (;;) {
        while (q_len == 0 && !shutting_down)
            pthread_cond_wait(&work_avail, &pool_lock);
        if (q_len == 0)
            break;      /* shutting down and drained */

        struct task t = queue[q_head];

        q_head = (q_head + 1) % q_cap;
        q_len--;
        in_flight++;
        pthread_mutex_unlock(&pool_lock);

        t.fn(t.arg);

        pthread_mutex_lock(&pool_lock);
        in_flight--;
        if (q_len == 0 && in_flight == 0)
            pthread_cond_signal(&batch_done);
    }
    pthread_mutex_unlock(&pool_lock);

    chopin_quote_thread_cleanup();
    chopin_copydata_thread_cleanup();
    return NULL;
}

/* Callers hold pool_lock. */
static void
start_workers(void)
{
    int want = chopin_pool_workers();

    while (n_workers < want) {
        if (pthread_create(&workers[n_workers], NULL, worker_main,
                           NULL) != 0)
            break;      /* run with what we got; 0 started = serial */
        n_workers++;
    }
}

/* Caller holds pool_lock. */
static void
queue_reserve(size_t n)
{
    if (q_len + n + 1 > q_cap) {
        size_t want = q_cap ? q_cap : 64;

        while (want < q_len + n + 1)
            want *= 2;

        struct task *nq = chopin_xmalloc(want * sizeof *nq);

        for (size_t i = 0; i < q_len; i++)
            nq[i] = queue[(q_head + i) % q_cap];
        free(queue);
        queue = nq;
        q_head = 0;
        q_cap = want;
    }
}

static void
submit_common(chopin_pool_fn fn, void *const *args, size_t n, bool front)
{
    if (n == 0)
        return;
    pthread_mutex_lock(&pool_lock);
    queue_reserve(n);
    if (front) {
        for (size_t i = n; i-- > 0; ) {
            q_head = (q_head + q_cap - 1) % q_cap;
            queue[q_head].fn = fn;
            queue[q_head].arg = args[i];
            q_len++;
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            queue[(q_head + q_len) % q_cap].fn = fn;
            queue[(q_head + q_len) % q_cap].arg = args[i];
            q_len++;
        }
    }
    shutting_down = false;
    start_workers();
    /* One handoff per batch - the batched contract. Small batches
       signal exactly n times instead of waking every parked worker
       (the broadcast storm was 36% of the warm kernel-tree
       profile). */
    if (n >= (size_t)n_workers) {
        pthread_cond_broadcast(&work_avail);
    } else {
        for (size_t i = 0; i < n; i++)
            pthread_cond_signal(&work_avail);
    }
    pthread_mutex_unlock(&pool_lock);
}

void
chopin_pool_submit(chopin_pool_fn fn, void *const *args, size_t n)
{
    submit_common(fn, args, n, false);
}

void
chopin_pool_submit_front(chopin_pool_fn fn, void *const *args, size_t n)
{
    submit_common(fn, args, n, true);
}

static void
join_workers_locked(void)
{
    shutting_down = true;
    pthread_cond_broadcast(&work_avail);
    while (n_workers > 0) {
        pthread_t t = workers[n_workers - 1];

        n_workers--;
        pthread_mutex_unlock(&pool_lock);
        pthread_join(t, NULL);
        pthread_mutex_lock(&pool_lock);
    }
}

void
chopin_pool_drain(void)
{
    pthread_mutex_lock(&pool_lock);
    while (q_len > 0 || in_flight > 0)
        pthread_cond_wait(&batch_done, &pool_lock);
    if (ephemeral && n_workers > 0)
        join_workers_locked();
    pthread_mutex_unlock(&pool_lock);
}

void
chopin_pool_shutdown(void)
{
    pthread_mutex_lock(&pool_lock);
    while (q_len > 0 || in_flight > 0)
        pthread_cond_wait(&batch_done, &pool_lock);
    join_workers_locked();
    free(queue);
    queue = NULL;
    q_cap = q_head = q_len = 0;
    pthread_mutex_unlock(&pool_lock);
}
