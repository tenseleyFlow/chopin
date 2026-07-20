#ifndef CHOPIN_META_H
#define CHOPIN_META_H

#include <stdbool.h>
#include <sys/stat.h>

#include "options.h"

/* The fd-based metadata tail for regular-file copies (gnu-cp-analysis
   3.1): timestamps -> ownership (set_owner pre-narrow, group-only
   retry) -> xattr (tolerance ladder) -> mode last (narrow-then-widen
   against the cached umask). Name-based variants for symlinks/dirs
   arrive in sprints 05/06.

   Returns false when the FILE must fail (require_preserve semantics);
   soft failures diagnose and continue. */
bool chopin_apply_meta_fd(int src_fd, const char *src_name,
                          int dest_fd, const char *dst_name,
                          const struct stat *src_sb,
                          const struct stat *dst_sb, bool new_dst,
                          mode_t dst_mode, mode_t omitted_permissions,
                          mode_t extra_permissions,
                          const struct chopin_options *x);

mode_t chopin_cached_umask(void);

/* Benign chown-failure test (EPERM/EINVAL/EACCES without
   privileges); reads errno. */
bool chopin_chown_failure_ok(void);

#endif
