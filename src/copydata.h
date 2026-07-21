#ifndef CHOPIN_COPYDATA_H
#define CHOPIN_COPYDATA_H

#include <stdbool.h>
#include <sys/stat.h>

#include "options.h"

/* --debug vocabularies (copy-file-data.c:134-166). */
struct chopin_copy_debug {
    const char *offload;    /* unknown no yes avoided unsupported */
    const char *reflink;    /* unknown no yes avoided unsupported */
    const char *sparse;     /* unknown no zeros SEEK_HOLE
                               "SEEK_HOLE + zeros" */
};

/* The engine ladder below FICLONE (gnu-cp-analysis.md 2.6):
   copy_file_range offload with the full fallback matrix (EFBIG
   mid-stream carve-out, CLONENOTSUP set, /proc 0-return, EINTR),
   SEEK_DATA/SEEK_HOLE extent walking, zero-detection hole punching,
   trailing-hole handling. DEV-005: a failed mid-file create_hole
   propagates failure (GNU exits 0 with a truncated dest).
   CHOPIN_FORCE_SCALAR routes everything to plain read/write. */
bool chopin_copy_file_data(int src_fd, const struct stat *src_sb,
                           const char *src_name, int dest_fd,
                           const struct stat *dst_sb,
                           const char *dst_name,
                           const struct chopin_options *x,
                           struct chopin_copy_debug *debug);

/* FICLONE (Linux) / fclonefileat (macOS) with chopin's failed-probe
   cache per (src_dev, dst_dev): 0 cloned, positive errno = attempted
   and failed, -1 = cache said don't bother (REFLINK_ALWAYS bypasses
   the cache). CHOPIN_DEBUG_STATS=1 prints probe/hit counters at
   exit. */
int chopin_clone_file(int dest_fd, int src_fd, dev_t src_dev,
                      dev_t dst_dev, bool new_dst,
                      const struct chopin_options *x);

/* Sprint 09: prime lazy state before any pool worker exists; free a
   worker thread's persistent copy buffer at pool teardown. */
void chopin_copydata_init(void);
void chopin_copydata_thread_cleanup(void);

/* Sprint 09 chunked dispatch: true when a clone attempt on this
   device pair is certain to fail (memoized or impossible). */
bool chopin_clone_pair_known_unsupported(dev_t src, dev_t dst);

#endif
