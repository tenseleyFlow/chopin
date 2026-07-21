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
   soft failures diagnose and continue.

   cur_sb, when non-NULL, is the dest's CURRENT stat: ownership is
   skipped when it already matches the source (GNU copy.c:1071
   SAME_OWNER_AND_GROUP guard - "avoid calling chown if we know it's
   not necessary"). NULL preserves the always-chown behavior (GNU's
   new-dir site). */
bool chopin_apply_meta_fd(int src_fd, const char *src_name,
                          int dest_fd, const char *dst_name,
                          const struct stat *src_sb,
                          const struct stat *dst_sb,
                          const struct stat *cur_sb, bool new_dst,
                          mode_t dst_mode, mode_t omitted_permissions,
                          mode_t extra_permissions,
                          const struct chopin_options *x);

/* Name-based tail for directories (post-order: after contents).
   restore_mode forces the final chmod even without preserve flags
   (the S_IRWXU temporary widen). src_fd < 0 skips the xattr pass. */
/* Fully name-based tail (*at calls only): special files, whose
   open would block or fail. No xattr pass. */
bool chopin_apply_meta_name(int dst_dirfd, const char *dst_relname,
                            const char *dst_name,
                            const struct stat *src_sb, bool new_dst,
                            mode_t dst_mode, mode_t omitted_permissions,
                            const struct chopin_options *x);

bool chopin_apply_meta_dir(const char *src_name, int dst_dirfd,
                           const char *dst_relname, const char *dst_name,
                           const struct stat *src_sb, bool new_dst,
                           mode_t dst_mode, mode_t omitted_permissions,
                           bool restore_mode,
                           const struct chopin_options *x);

mode_t chopin_cached_umask(void);

/* Benign chown-failure test (EPERM/EINVAL/EACCES without
   privileges); reads errno. */
bool chopin_chown_failure_ok(void);

#endif
