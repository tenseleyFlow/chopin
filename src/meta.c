#include "meta.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.h"
#include "quote.h"
#include "util.h"

#if CHOPIN_HAVE_GETXATTR
#include <sys/xattr.h>
#endif

mode_t
chopin_cached_umask(void)
{
    static mode_t mask;
    static bool cached;

    if (!cached) {
        mask = umask(0);
        umask(mask);
        cached = true;
    }
    return mask;
}

/* chown_failure_ok (copy.c:2853-2868): benign errnos without
   privileges. */
bool
chopin_chown_failure_ok(void)
{
    return (errno == EPERM || errno == EINVAL || errno == EACCES)
        && geteuid() != 0;
}

/* set_owner (copy.c:482-534) reduced to the non-ACL build: pre-narrow
   an existing dest before chown when its old mode carries bits the
   new mode lacks or set-id bits. Returns 1 ok, 0 soft-failed (strip
   set-id bits from the mode to apply), -1 fail-the-file. */
static int
set_owner_fd(int dest_fd, const char *dst_name,
             const struct stat *src_sb, const struct stat *dst_sb,
             bool new_dst, mode_t dst_mode,
             const struct chopin_options *x)
{
    if (!new_dst && dst_sb != NULL) {
        mode_t old_mode = dst_sb->st_mode & 07777;
        mode_t new_mode = dst_mode;

        if ((old_mode & ~new_mode) != 0
            || (old_mode & (S_ISUID | S_ISGID | S_ISVTX)) != 0) {
            if (fchmod(dest_fd, old_mode & new_mode & S_IRWXU) != 0) {
                if (!chopin_chown_failure_ok()) {
                    chopin_error(errno, "clearing permissions for %s",
                                 chopin_quoteaf(dst_name));
                    return -(int)x->require_preserve;
                }
                return 0;
            }
        }
    }

    if (fchown(dest_fd, src_sb->st_uid, src_sb->st_gid) == 0)
        return 1;
    if (chopin_chown_failure_ok()) {
        /* Group-only retry, silently. */
        if (fchown(dest_fd, (uid_t)-1, src_sb->st_gid) == 0)
            return 1;
        if (chopin_chown_failure_ok())
            return 0;
    }
    chopin_error(errno, "failed to preserve ownership for %s",
                 chopin_quoteaf(dst_name));
    return x->require_preserve ? -1 : 0;
}

/* copy_attr's verbosity ladder (copy.c:322-364, copy.h:237-243):
   all = --attributes-only or --preserve=xattr; some = plain -p
   context (warn on everything except ENOTSUP/ENODATA); -a is fully
   silent. security.selinux excluded. /etc/xattr.conf is NOT read
   (doc/deviations.md DEV-006; fixtures use user.* names). */
static bool
copy_xattrs_fd(int src_fd, const char *src_name, int dest_fd,
               const char *dst_name, const struct chopin_options *x)
{
#if CHOPIN_HAVE_GETXATTR
    bool all_errors = !x->data_copy_required || x->require_preserve_xattr;
    bool some_errors = !all_errors && !x->reduce_diagnostics;
    bool ok = true;
    char names[65536];
    static char value[65536];
    ssize_t len = flistxattr(src_fd, names, sizeof names);

    if (len < 0) {
        if (all_errors
            || (some_errors && errno != ENOTSUP && errno != ENODATA))
            chopin_error(errno, "listing attributes of %s",
                         chopin_quoteaf(src_name));
        return !x->require_preserve_xattr;
    }
    for (char *p = names; p < names + len; p += strlen(p) + 1) {
        if (strcmp(p, "security.selinux") == 0)
            continue;
        ssize_t vlen = fgetxattr(src_fd, p, value, sizeof value);
        if (vlen < 0) {
            if (all_errors
                || (some_errors && errno != ENOTSUP && errno != ENODATA))
                chopin_error(errno, "getting attribute %s of %s",
                             chopin_quoteaf_n(0, p),
                             chopin_quoteaf_n(1, src_name));
            ok = false;
            continue;
        }
        if (fsetxattr(dest_fd, p, value, (size_t)vlen, 0) != 0) {
            if (all_errors
                || (some_errors && errno != ENOTSUP && errno != ENODATA))
                chopin_error(errno, "setting attribute %s of %s",
                             chopin_quoteaf_n(0, p),
                             chopin_quoteaf_n(1, dst_name));
            ok = false;
        }
    }
    return ok ? true : !x->require_preserve_xattr;
#else
    (void)src_fd;
    (void)src_name;
    (void)dest_fd;
    /* Non-Linux xattr backends land in sprint 04's FreeBSD/macOS
       follow-up; resolution step 13 already fatals require_* here. */
    (void)dst_name;
    (void)x;
    return true;
#endif
}

bool
chopin_apply_meta_fd(int src_fd, const char *src_name,
                     int dest_fd, const char *dst_name,
                     const struct stat *src_sb,
                     const struct stat *dst_sb, bool new_dst,
                     mode_t dst_mode, mode_t omitted_permissions,
                     mode_t extra_permissions,
                     const struct chopin_options *x)
{
    bool return_val = true;
    mode_t mode_to_apply = dst_mode;

    /* 1. Timestamps (copy.c:1052-1067). */
    if (x->preserve_timestamps) {
        struct timespec ts[2];

#if CHOPIN_HAVE_ST_MTIM
        ts[0] = src_sb->st_atim;
        ts[1] = src_sb->st_mtim;
#elif CHOPIN_HAVE_ST_MTIMESPEC
        ts[0] = src_sb->st_atimespec;
        ts[1] = src_sb->st_mtimespec;
#else
        ts[0].tv_sec = src_sb->st_atime;
        ts[0].tv_nsec = 0;
        ts[1].tv_sec = src_sb->st_mtime;
        ts[1].tv_nsec = 0;
#endif
        if (futimens(dest_fd, ts) != 0) {
            chopin_error(errno, "preserving times for %s",
                         chopin_quoteaf(dst_name));
            if (x->require_preserve)
                return_val = false;
        }
    }

    /* 2. Ownership before xattr (chown clears capabilities) and
       before chmod (chown clears set-id bits for non-root). */
    if (x->preserve_ownership) {
        int r = set_owner_fd(dest_fd, dst_name, src_sb, dst_sb, new_dst,
                             dst_mode, x);
        if (r < 0)
            return_val = false;
        else if (r == 0)
            mode_to_apply &= (mode_t)~(S_ISUID | S_ISGID | S_ISVTX);
    }

    /* 3. xattr. */
    if (x->preserve_xattr) {
        if (!copy_xattrs_fd(src_fd, src_name, dest_fd, dst_name, x))
            return_val = false;
    }

    /* 4. Mode last (copy.c:1098-1127); xcopy_acl reduces to fchmod on
       the pinned no-ACL build (DEV-006). */
    if (x->preserve_mode) {
        if (fchmod(dest_fd, mode_to_apply) != 0) {
            chopin_error(errno, "preserving permissions for %s",
                         chopin_quoteaf(dst_name));
            if (x->require_preserve)
                return_val = false;
        }
    } else if (x->explicit_no_preserve_mode && new_dst) {
        if (fchmod(dest_fd, 0666 & ~chopin_cached_umask()) != 0) {
            chopin_error(errno, "preserving permissions for %s",
                         chopin_quoteaf(dst_name));
            if (x->require_preserve)
                return_val = false;
        }
    } else if (omitted_permissions != 0 || extra_permissions != 0) {
        /* Re-widen what creation narrowed, through the umask; a
           chmod-free minimal profile stays chmod-free (9.1). */
        if (fchmod(dest_fd, dst_mode & ~chopin_cached_umask()) != 0) {
            chopin_error(errno, "preserving permissions for %s",
                         chopin_quoteaf(dst_name));
            if (x->require_preserve)
                return_val = false;
        }
    }

    return return_val;
}
