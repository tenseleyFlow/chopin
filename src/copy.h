#ifndef CHOPIN_COPY_H
#define CHOPIN_COPY_H

#include <stdbool.h>

#include "options.h"

/* The copy_internal spine (gnu-cp-analysis.md 2.1), sprint 02 scope:
   regular-file path for command-line arguments. dst is addressed as
   (dst_dirfd, dst_relname) exactly as GNU does; dst_name is the full
   name for diagnostics. nonexistent_dst < 0 means "known absent
   except possibly as a dangling symlink" (skips the dst stat). */
/* Prime lazy state (verify flag, stats flag, umask cache) before any
   pool worker exists - sprint 09. */
void chopin_copy_init(void);

bool chopin_copy(const char *src_name, const char *dst_name,
                 int dst_dirfd, const char *dst_relname,
                 int nonexistent_dst, const struct chopin_options *x,
                 bool *copy_into_self);

#include <sys/stat.h>

/* Exported for the unit matrix driver (gnu-cp-analysis.md 2.3). */
bool chopin_same_file_ok(const char *src_name, const struct stat *src_sb,
                         int dst_dirfd, const char *dst_relname,
                         const struct stat *dst_sb,
                         const struct chopin_options *x,
                         bool *return_now, bool *diagnosed);
int chopin_same_nameat(int dirfd_a, const char *a,
                       int dirfd_b, const char *b);

#endif
