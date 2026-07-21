#include "parallel.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/sysmacros.h>
#endif

#if defined(__linux__)
#include <sys/statfs.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/mount.h>
#endif

#include "config.h"
#include "pool.h"
#include "util.h"

/* ---- Floors -------------------------------------------------------
   CHOPIN_PARALLEL_MIN=n sets both floors to n (n=1: every eligible
   payload dispatches - the tests' pin). Unset: 4 files AND 8 MiB
   aggregate before the pool wakes, so tiny copies never pay thread
   costs. Defaults are sprint 10's to tune. */

static bool floors_parsed;
static long file_floor = 4;
static long long byte_floor = 8 << 20;
static long seen_files;
static long long seen_bytes;
/* Fed by the prefetch cascade (worker threads): what the tree
   actually holds, discovered well ahead of the spine - so a big
   tree activates the pool at its head instead of 8 MiB in. */
static _Atomic long scout_files;
static _Atomic long long scout_bytes;

static void
parse_floors(void)
{
    const char *e = getenv("CHOPIN_PARALLEL_MIN");

    if (e != NULL && *e != '\0') {
        long v = strtol(e, NULL, 10);

        if (v >= 1) {
            file_floor = v;
            byte_floor = v > 1 ? v : 0;
        }
    }
    floors_parsed = true;
}

void
chopin_parallel_note_eligible(off_t bytes)
{
    if (!floors_parsed)
        parse_floors();
    seen_files++;
    seen_bytes += bytes;
}

bool
chopin_parallel_active(void)
{
    if (!floors_parsed)
        parse_floors();
    if (chopin_pool_workers() <= 0)
        return false;
    if (seen_files >= file_floor && seen_bytes >= byte_floor)
        return true;
    return atomic_load(&scout_files) >= file_floor
        && atomic_load(&scout_bytes) >= byte_floor;
}

/* ---- Device-pair classing (overview s7) ---------------------------
   Rotational pairs and network filesystems stay serial: fan-out on
   spinning rust seeks itself to death, and network round-trips
   swamp thread wins - EXCEPT same-device network pairs, where
   copy_file_range offloads server-side and the payload is cheap
   (xcp's 10x lesson). Rotational-ness resolves dm/md/loop stacks
   through the sysfs slaves directories: an LVM volume on spinning
   rust must class rotational. Unknown local devices class
   non-rotational (Darwin/FreeBSD have no sysfs; their dev boxes
   are SSDs). */

static bool
fs_is_network(int dirfd, const char *path)
{
#if defined(__linux__)
    struct statfs sfs;

    if (path != NULL) {
        if (statfs(path, &sfs) != 0)
            return false;
    } else if (fstatfs(dirfd, &sfs) != 0) {
        return false;
    }
    switch ((unsigned long)sfs.f_type) {
    case 0x6969UL:      /* NFS */
    case 0x517BUL:      /* SMB */
    case 0xFF534D42UL:  /* CIFS */
    case 0xFE534D42UL:  /* SMB2 */
    case 0x65735546UL:  /* FUSE */
    case 0x01021997UL:  /* 9P */
        return true;
    default:
        return false;
    }
#else
    (void)dirfd;
    (void)path;
    return false;
#endif
}

#if defined(__linux__)
/* Any rotational device under this /sys/class/block node? Recurses
   through slaves/ for dm/md/loop stacks. */
static int
sys_block_rotational(const char *node, int depth)
{
    char p[512];
    FILE *f;

    if (depth > 6)
        return 0;
    snprintf(p, sizeof p, "/sys/class/block/%s/queue/rotational", node);
    f = fopen(p, "re");
    if (f != NULL) {
        int c = fgetc(f);

        fclose(f);
        if (c == '1')
            return 1;
    }

    /* Partition nodes have no queue/; strip the partition suffix by
       consulting the parent link is overkill - slaves/ carries the
       stacking cases and whole-disk queue answers the rest. */
    snprintf(p, sizeof p, "/sys/class/block/%s/slaves", node);

    DIR *d = opendir(p);

    if (d != NULL) {
        struct dirent *de;
        int rot = 0;

        while (!rot && (de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            rot = sys_block_rotational(de->d_name, depth + 1);
        }
        closedir(d);
        return rot;
    }
    return 0;
}

static bool
dev_rotational(dev_t dev)
{
    char link[64], target[512];
    ssize_t n;

    snprintf(link, sizeof link, "/sys/dev/block/%u:%u",
             major(dev), minor(dev));
    n = readlink(link, target, sizeof target - 1);
    if (n <= 0)
        return false;
    target[n] = '\0';

    /* target ends .../block/sda/sda2 or .../block/nvme0n1 - walk the
       final components against /sys/class/block. */
    char *base = strrchr(target, '/');

    if (base == NULL)
        return false;
    if (sys_block_rotational(base + 1, 0))
        return true;

    *base = '\0';

    char *parent = strrchr(target, '/');

    if (parent != NULL && strcmp(parent + 1, "block") != 0)
        return sys_block_rotational(parent + 1, 0);
    return false;
}
#else
static bool
dev_rotational(dev_t dev)
{
    (void)dev;
    return false;
}
#endif

static dev_t dst_dev;
static bool dst_known;
static bool dst_rot;
static bool dst_net;

void
chopin_parallel_class_dst(int dst_dirfd)
{
    struct stat sb;
    int fd = dst_dirfd;
    int opened = -1;

    if (dst_dirfd == AT_FDCWD) {
        opened = open(".", O_RDONLY);
        if (opened < 0)
            return;
        fd = opened;
    }
    if (fstat(fd, &sb) == 0) {
        dst_dev = sb.st_dev;
        dst_rot = dev_rotational(sb.st_dev);
        dst_net = fs_is_network(fd, NULL);
        dst_known = true;
    }
    if (opened >= 0)
        close(opened);
}

/* Memo per source device. */
struct src_class {
    dev_t dev;
    bool serial;
};
static struct src_class *src_classes;
static size_t n_src_classes, cap_src_classes;

bool
chopin_parallel_pair_ok(dev_t src_dev, const char *src_name)
{
    if (!dst_known)
        return false;

    for (size_t i = 0; i < n_src_classes; i++)
        if (src_classes[i].dev == src_dev)
            return !src_classes[i].serial && !dst_rot
                && (!dst_net || src_dev == dst_dev);

    bool src_rot = dev_rotational(src_dev);
    bool src_net = fs_is_network(AT_FDCWD, src_name);
    bool serial = src_rot || (src_net && src_dev != dst_dev);

    if (n_src_classes == cap_src_classes) {
        cap_src_classes = cap_src_classes ? cap_src_classes * 2 : 4;
        src_classes = chopin_xrealloc(src_classes,
                                      cap_src_classes
                                          * sizeof *src_classes);
    }
    src_classes[n_src_classes].dev = src_dev;
    src_classes[n_src_classes].serial = serial;
    n_src_classes++;

    return !serial && !dst_rot && (!dst_net || src_dev == dst_dev);
}

/* ---- Ordered slots + pending set ---------------------------------- */

enum slot_kind { SLOT_SYNC, SLOT_PAYLOAD };

struct slot {
    enum slot_kind kind;
    struct slot *next;
    struct chopin_errcap cap;
    /* payload slots */
    bool (*run)(void *);
    void (*join)(void *, bool);
    void *payload;
    const char *dst_path;   /* owned by payload; valid until join */
    bool ok;
};

static struct slot *slot_head, *slot_tail;
static struct slot *cur_sync;
static bool sticky_failure;     /* prompt-flush consumed a failed batch */

/* Pending dest paths: FNV-1a open addressing, cleared whole at each
   barrier. */
static const char **pend_tab;
static size_t pend_cap, pend_len;

static size_t
str_hash(const char *s)
{
    size_t h = 1469598103934665603ull;

    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ull;
    }
    return h;
}

static void
pend_add(const char *path)
{
    if ((pend_len + 1) * 2 > pend_cap) {
        size_t ncap = pend_cap ? pend_cap * 2 : 64;
        const char **nt = chopin_xmalloc(ncap * sizeof *nt);

        memset(nt, 0, ncap * sizeof *nt);
        for (size_t i = 0; i < pend_cap; i++)
            if (pend_tab[i] != NULL) {
                size_t j = str_hash(pend_tab[i]) & (ncap - 1);

                while (nt[j] != NULL)
                    j = (j + 1) & (ncap - 1);
                nt[j] = pend_tab[i];
            }
        free(pend_tab);
        pend_tab = nt;
        pend_cap = ncap;
    }

    size_t i = str_hash(path) & (pend_cap - 1);

    while (pend_tab[i] != NULL) {
        if (strcmp(pend_tab[i], path) == 0)
            return;
        i = (i + 1) & (pend_cap - 1);
    }
    pend_tab[i] = path;
    pend_len++;
}

bool
chopin_parallel_pending_path(const char *path)
{
    if (pend_len == 0)
        return false;

    size_t i = str_hash(path) & (pend_cap - 1);

    while (pend_tab[i] != NULL) {
        if (strcmp(pend_tab[i], path) == 0)
            return true;
        i = (i + 1) & (pend_cap - 1);
    }
    return false;
}

static struct slot *
slot_append(enum slot_kind kind)
{
    struct slot *s = chopin_xmalloc(sizeof *s);

    memset(s, 0, sizeof *s);
    s->kind = kind;
    if (slot_tail != NULL)
        slot_tail->next = s;
    else
        slot_head = s;
    slot_tail = s;
    return s;
}

/* Worker entry: run the payload with diagnostics captured into the
   slot. The ok/cap writes are read by the spine only after
   chopin_pool_drain - the pool lock orders them. */
static void
payload_trampoline(void *arg)
{
    struct slot *s = arg;

    chopin_error_capture(&s->cap);
    s->ok = s->run(s->payload);
    chopin_error_capture(NULL);
}

/* ---- Cold-metadata subtree prefetch (sprint 10B) ------------------
   The cold-swarm profile showed the spine's serial newfstatat paying
   the full cold metadata latency per file (cold spine walk alone:
   1.23s over 50k; warm: 0.12s). This is fcp's parallel traversal
   recast as ADVISORY cache warming: a worker task opens one
   directory, stats every entry to warm dentries/inodes, and
   self-submits a task per child directory - the cascade walks the
   whole subtree ahead of the spine while touching nothing the spine
   reasons about. No result is kept, no ordering exists, and if the
   spine outruns the cascade it pays cold exactly as before.
   Front-of-queue so queued copy payloads cannot starve it. */
struct prefetch_task {
    char *path;
    int depth;
};

static void
prefetch_run(void *arg)
{
    struct prefetch_task *t = arg;
    struct stat sb;

    if (fstatat(AT_FDCWD, t->path, &sb, AT_SYMLINK_NOFOLLOW) == 0
        && S_ISDIR(sb.st_mode) && t->depth < 32) {
        DIR *d = opendir(t->path);

        if (d != NULL) {
            size_t plen = strlen(t->path);
            struct dirent *de;
            void *kids[64];
            size_t nkids = 0;

            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.'
                    && (de->d_name[1] == '\0'
                        || (de->d_name[1] == '.'
                            && de->d_name[2] == '\0')))
                    continue;

                size_t nl = strlen(de->d_name);
                char *p = chopin_xmalloc(plen + 1 + nl + 1);
                struct stat csb;

                memcpy(p, t->path, plen);
                p[plen] = '/';
                memcpy(p + plen + 1, de->d_name, nl + 1);
                if (fstatat(AT_FDCWD, p, &csb,
                            AT_SYMLINK_NOFOLLOW) != 0) {
                    free(p);
                    continue;
                }
                if (S_ISREG(csb.st_mode)) {
                    atomic_fetch_add(&scout_files, 1);
                    atomic_fetch_add(&scout_bytes, csb.st_size);
                }
                if (S_ISDIR(csb.st_mode)) {
                    struct prefetch_task *ct =
                        chopin_xmalloc(sizeof *ct);

                    ct->path = p;
                    ct->depth = t->depth + 1;
                    kids[nkids++] = ct;
                    if (nkids == 64) {
                        chopin_pool_submit_front(prefetch_run, kids,
                                                 nkids);
                        nkids = 0;
                    }
                } else {
                    free(p);    /* file: the stat itself was the work */
                }
            }
            closedir(d);
            if (nkids > 0)
                chopin_pool_submit_front(prefetch_run, kids, nkids);
        }
    }
    free(t->path);
    free(t);
}

void
chopin_parallel_prefetch_tree(const char *dir)
{
    /* Deliberately floor-free: the floors gate DISPATCH cost on tiny
       copies, but a recursive dir operand is the pool's natural
       shape and the cascade must run ahead of the very first cold
       stats - waiting for the byte floor would leave the head of
       the tree serial-cold. */
    if (chopin_pool_workers() <= 0)
        return;

    struct prefetch_task *t = chopin_xmalloc(sizeof *t);
    void *arg = t;

    t->path = chopin_xstrdup(dir);
    t->depth = 0;
    chopin_pool_submit_front(prefetch_run, &arg, 1);
}

/* Batched handoff: dispatches accumulate locally and reach the pool
   256 at a time (one lock + one broadcast per flush), or all at once
   at a barrier. Never per-file signaling; the fat batch keeps the
   futex churn out of the profile. */
enum { HANDOFF_BATCH = 256 };
static void *handoff[HANDOFF_BATCH];
static size_t n_handoff;

static void
flush_handoff(void)
{
    if (n_handoff > 0) {
        chopin_pool_submit(payload_trampoline, handoff, n_handoff);
        n_handoff = 0;
    }
}

/* Deferred-barrier memory bound (sprint 10B): with no-op dir
   metadata the per-directory barriers stop firing, so slots and
   pending strings accumulate; drain when the batch grows past this
   cap. Any spine point is a safe drain point. */
enum { SLOT_SOFT_CAP = 8192 };
static size_t slots_since_barrier;

void
chopin_parallel_dispatch(bool (*run)(void *), void (*join)(void *, bool),
                         void *payload, const char *dst_path)
{
    if (slots_since_barrier >= SLOT_SOFT_CAP)
        chopin_parallel_drain_keep();

    struct slot *s = slot_append(SLOT_PAYLOAD);

    slots_since_barrier++;

    s->run = run;
    s->join = join;
    s->payload = payload;
    s->dst_path = dst_path;
    pend_add(dst_path);

    handoff[n_handoff++] = s;
    if (n_handoff == HANDOFF_BATCH)
        flush_handoff();

    /* Spine stderr from here to the next dispatch belongs AFTER this
       payload's: rotate the capture to a fresh sync slot. */
    cur_sync = slot_append(SLOT_SYNC);
    chopin_error_capture(&cur_sync->cap);
}

bool
chopin_parallel_barrier(void)
{
    bool all = !sticky_failure;

    sticky_failure = false;
    if (slot_head == NULL)
        return all;

    chopin_error_capture(NULL);
    cur_sync = NULL;
    flush_handoff();
    chopin_pool_drain();

    struct slot *s = slot_head;

    while (s != NULL) {
        struct slot *next = s->next;

        chopin_errcap_flush(&s->cap);
        free(s->cap.buf);
        if (s->kind == SLOT_PAYLOAD) {
            if (s->join != NULL)
                s->join(s->payload, s->ok);
            all &= s->ok;
        }
        free(s);
        s = next;
    }
    slot_head = slot_tail = NULL;
    slots_since_barrier = 0;
    if (pend_len > 0) {
        memset(pend_tab, 0, pend_cap * sizeof *pend_tab);
        pend_len = 0;
    }
    return all;
}

void
chopin_parallel_drain_keep(void)
{
    if (!chopin_parallel_barrier())
        sticky_failure = true;
}

void
chopin_parallel_flush_for_prompt(void)
{
    chopin_parallel_drain_keep();
}

void
chopin_parallel_shutdown(void)
{
    chopin_pool_shutdown();
    free(pend_tab);
    pend_tab = NULL;
    pend_cap = pend_len = 0;
    free(src_classes);
    src_classes = NULL;
    n_src_classes = cap_src_classes = 0;
}
