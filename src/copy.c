#include "copy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "copydata.h"
#include "quote.h"
#include "util.h"

/* Sprint 02: copy_internal (2.1 items 1-8, 15) + copy_reg (2.4) for
   the regular-file command-line path with the scalar engine. Backup
   machinery, hash-table guards, link/dir/special dispatch, and the
   metadata engine arrive in sprints 03-07; their sites are marked. */

#define SAME_INODE(a, b) \
    ((a).st_ino == (b).st_ino && (a).st_dev == (b).st_dev)

static bool
should_dereference(const struct chopin_options *x, bool command_line_arg)
{
    return x->dereference == CHOPIN_DEREF_ALWAYS
        || (x->dereference == CHOPIN_DEREF_COMMAND_LINE_ARGUMENTS
            && command_line_arg);
}

/* gnulib yesno: read one stdin line; affirmative = rpmatch, which in
   the C locale is a leading y/Y. */
static bool
yesno(void)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, stdin);
    bool yes = n > 0 && (line[0] == 'y' || line[0] == 'Y');

    free(line);
    return yes;
}

/* copy.c:1400-1410. */
static bool
writable_destination(int dst_dirfd, const char *dst_relname, mode_t mode)
{
    return S_ISLNK(mode)
        || faccessat(dst_dirfd, dst_relname, W_OK, AT_EACCESS) == 0;
}

/* strmode reduced: ls-style "-rwxrwxrwx"; GNU prints &perms[1]. */
static void
strmode10(mode_t mode, char perms[11])
{
    perms[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : '-';
    perms[1] = mode & S_IRUSR ? 'r' : '-';
    perms[2] = mode & S_IWUSR ? 'w' : '-';
    perms[3] = mode & S_ISUID ? (mode & S_IXUSR ? 's' : 'S')
                              : (mode & S_IXUSR ? 'x' : '-');
    perms[4] = mode & S_IRGRP ? 'r' : '-';
    perms[5] = mode & S_IWGRP ? 'w' : '-';
    perms[6] = mode & S_ISGID ? (mode & S_IXGRP ? 's' : 'S')
                              : (mode & S_IXGRP ? 'x' : '-');
    perms[7] = mode & S_IROTH ? 'r' : '-';
    perms[8] = mode & S_IWOTH ? 'w' : '-';
    perms[9] = mode & S_ISVTX ? (mode & S_IXOTH ? 't' : 'T')
                              : (mode & S_IXOTH ? 'x' : '-');
    perms[10] = '\0';
}

/* copy.c:1412-1438; cp wording (mv/--remove-destination/-f use the
   "replace" variant). */
static bool
overwrite_ok(const struct chopin_options *x, const char *dst_name,
             int dst_dirfd, const char *dst_relname,
             const struct stat *dst_sb)
{
    if (!writable_destination(dst_dirfd, dst_relname, dst_sb->st_mode)) {
        char perms[11];

        strmode10(dst_sb->st_mode, perms);
        fprintf(stderr,
                (x->unlink_dest_before_opening
                 || x->unlink_dest_after_failed_open)
                    ? "%s: replace %s, overriding mode %04lo (%s)? "
                    : "%s: unwritable %s (mode %04lo, %s); try anyway? ",
                chopin_prog, chopin_quoteaf(dst_name),
                (unsigned long)(dst_sb->st_mode & 07777), &perms[1]);
    } else {
        fprintf(stderr, "%s: overwrite %s? ",
                chopin_prog, chopin_quoteaf(dst_name));
    }
    return yesno();
}

/* --debug per-file line (copy-file-data.c emit_debug shape). The
   scalar era reports truthfully: nothing attempted. Byte-parity of
   VALUES vs GNU is deliberately deferred to sprint 07 (GNU's
   reflink=auto attempts FICLONE on every file); the SHAPE is final. */
struct copy_debug {
    const char *offload;
    const char *reflink;
    const char *sparse;
};

static void
emit_debug(const struct chopin_options *x, const struct copy_debug *d)
{
    if (!x->debug || x->hard_link || x->symbolic_link
        || !x->data_copy_required)
        return;
    printf("copy offload: %s, reflink: %s, sparse detection: %s\n",
           d->offload, d->reflink, d->sparse);
}

/* copy_reg per 2.4; scalar engine only this sprint. */
static bool
copy_reg(const char *src_name, const char *dst_name,
         int dst_dirfd, const char *dst_relname,
         const struct chopin_options *x, mode_t dst_mode, bool *new_dst,
         struct stat *src_sb)
{
    struct stat src_open_sb;
    struct stat sb;
    int source_desc;
    int dest_desc = -1;
    bool return_val = true;
    struct copy_debug debug = { "no", "no", "no" };
    int open_flags = O_RDONLY
        | (x->dereference == CHOPIN_DEREF_NEVER ? O_NOFOLLOW : 0);

    source_desc = open(src_name, open_flags);
    if (source_desc < 0) {
        chopin_error(errno, "cannot open %s for reading",
                     chopin_quoteaf(src_name));
        return false;
    }
    if (fstat(source_desc, &src_open_sb) != 0) {
        chopin_error(errno, "cannot fstat %s", chopin_quoteaf(src_name));
        return_val = false;
        goto close_src_desc;
    }
    /* Replaced-while-copying check (quirk 18): the pre-open stat and
       the post-open fstat must agree on identity. */
    if (!SAME_INODE(*src_sb, src_open_sb)) {
        chopin_error(0, "skipping file %s, as it was replaced while "
                        "being copied", chopin_quoteaf(src_name));
        return_val = false;
        goto close_src_desc;
    }
    *src_sb = src_open_sb;

    if (!*new_dst) {
        int flags = O_WRONLY | (x->data_copy_required ? O_TRUNC : 0);

        dest_desc = openat(dst_dirfd, dst_relname, flags);
        /* set_file_security_ctx: non-SELinux, no-op. */

        if (dest_desc < 0 && errno != ENOENT
            && x->unlink_dest_after_failed_open) {
            if (unlinkat(dst_dirfd, dst_relname, 0) != 0) {
                chopin_error(errno, "cannot remove %s",
                             chopin_quoteaf(dst_name));
                return_val = false;
                goto close_src_desc;
            }
            if (x->verbose)
                printf("removed %s\n", chopin_quoteaf(dst_name));
            errno = ENOENT;
        }
        if (dest_desc < 0 && errno == ENOENT)
            *new_dst = true;
    }

    if (*new_dst) {
        /* omitted_permissions is folded by the caller into dst_mode;
           extra_permissions (preserve_xattr sans privileges) lands in
           sprint 04. */
        mode_t open_mode = dst_mode;

        dest_desc = openat(dst_dirfd, dst_relname,
                           O_WRONLY | O_CREAT | O_EXCL, open_mode);
        if (dest_desc < 0 && errno == EEXIST) {
            /* A dangling/other symlink appeared: refuse unless
               POSIXLY_CORRECT permits writing through it (quirk 11). */
            struct stat lsb;

            if (fstatat(dst_dirfd, dst_relname, &lsb,
                        AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(lsb.st_mode)) {
                if (x->open_dangling_dest_symlink) {
                    dest_desc = openat(dst_dirfd, dst_relname,
                                       O_WRONLY | O_CREAT, open_mode);
                } else {
                    chopin_error(0, "not writing through dangling symlink "
                                    "%s", chopin_quoteaf(dst_name));
                    return_val = false;
                    goto close_src_desc;
                }
            }
        }
        if (dest_desc < 0 && errno == EISDIR
            && *dst_name && dst_name[strlen(dst_name) - 1] == '/')
            errno = ENOTDIR;
        if (dest_desc < 0) {
            chopin_error(errno, "cannot create regular file %s",
                         chopin_quoteaf(dst_name));
            return_val = false;
            goto close_src_desc;
        }
    } else if (dest_desc < 0) {
        chopin_error(errno, "cannot create regular file %s",
                     chopin_quoteaf(dst_name));
        return_val = false;
        goto close_src_desc;
    }

    /* Engine ladder: FICLONE and copy_file_range land in sprint 07;
       everything routes scalar (plan reports engine=scalar). */

    if (fstat(dest_desc, &sb) != 0) {
        chopin_error(errno, "cannot fstat %s", chopin_quoteaf(dst_name));
        return_val = false;
        goto close_src_and_dst_desc;
    }

    if (x->data_copy_required) {
        if (!chopin_copy_file_data(source_desc, &src_open_sb, src_name,
                                   dest_desc, &sb, dst_name))
            return_val = false;
    }

    /* Metadata tail (times -> ownership -> xattr -> mode) arrives in
       sprint 04; bare cp applies none of it for regular copies. */

close_src_and_dst_desc:
    if (close(dest_desc) < 0) {
        chopin_error(errno, "failed to close %s", chopin_quoteaf(dst_name));
        return_val = false;
    }
close_src_desc:
    if (close(source_desc) < 0) {
        chopin_error(errno, "failed to close %s", chopin_quoteaf(src_name));
        return_val = false;
    }

    if (return_val)
        emit_debug(x, &debug);
    return return_val;
}

bool
chopin_copy(const char *src_name, const char *dst_name,
            int dst_dirfd, const char *dst_relname,
            int nonexistent_dst, const struct chopin_options *x,
            bool *copy_into_self)
{
    struct stat src_sb;
    struct stat dst_sb;
    bool new_dst = false;
    bool have_dst_sb = false;
    bool command_line_arg = true;   /* recursion arrives in sprint 06 */

    *copy_into_self = false;

    /* 2.1 item 2: stat source. */
    int fflags = should_dereference(x, command_line_arg)
        ? 0 : AT_SYMLINK_NOFOLLOW;
    if (fstatat(AT_FDCWD, src_name, &src_sb, fflags) != 0) {
        chopin_error(errno, "cannot stat %s", chopin_quoteaf(src_name));
        return false;
    }

    /* Item 3: directories need -R; the traversal engine is sprint 06. */
    if (S_ISDIR(src_sb.st_mode)) {
        if (!x->recursive) {
            chopin_error(0, "-r not specified; omitting directory %s",
                         chopin_quoteaf(src_name));
            return false;
        }
        chopin_error(0, "internal: directory copying arrives in sprint 06");
        return false;
    }

    /* Item 4: src_info duplicate guard needs >=2-source tables
       (sprint 03). */

    /* Item 5: dst stat policy. */
    bool use_lstat = x->symbolic_link || x->hard_link
        || x->backup_type != CHOPIN_BACKUP_NONE
        || x->unlink_dest_before_opening;

    if (nonexistent_dst < 0 && !use_lstat) {
        new_dst = true;
    } else {
        int dflags = use_lstat ? AT_SYMLINK_NOFOLLOW : 0;
        if (fstatat(dst_dirfd, dst_relname, &dst_sb, dflags) == 0) {
            have_dst_sb = true;
        } else if (errno == ENOENT) {
            new_dst = true;
        } else if (errno == ELOOP && x->unlink_dest_after_failed_open
                   && !use_lstat) {
            /* Proceed so the -f unlink can fire. */
        } else {
            chopin_error(errno, "cannot stat %s", chopin_quoteaf(dst_name));
            return false;
        }
    }

    /* Item 6: existing-dst decision ladder. */
    if (have_dst_sb) {
        /* same_file_ok: the 10-case matrix is sprint 03. The stub
           detects plain identity - GNU refuses, and an
           always-different stub would let `cp a a` truncate a
           (sprint 02 Amendment). */
        if (x->update != CHOPIN_UPDATE_NONE
            && x->update != CHOPIN_UPDATE_NONE_FAIL
            && SAME_INODE(src_sb, dst_sb)) {
            chopin_error(0, "%s and %s are the same file",
                         chopin_quoteaf_n(0, src_name),
                         chopin_quoteaf_n(1, dst_name));
            return false;
        }

        /* --update=older: nanosecond mtime compare. GNU truncates the
           source timestamp to the dest fs resolution when preserving
           times (UTIMECMP_TRUNCATE_SOURCE); the truncation nuance
           lands with the metadata engine (sprint 04 dev-note). */
        if (x->update == CHOPIN_UPDATE_OLDER && !S_ISDIR(src_sb.st_mode)) {
#if CHOPIN_HAVE_ST_MTIM
            bool older = dst_sb.st_mtim.tv_sec < src_sb.st_mtim.tv_sec
                || (dst_sb.st_mtim.tv_sec == src_sb.st_mtim.tv_sec
                    && dst_sb.st_mtim.tv_nsec < src_sb.st_mtim.tv_nsec);
#else
            bool older = dst_sb.st_mtime < src_sb.st_mtime;
#endif
            if (!older)
                return true;    /* remember_copied hook: sprint 05 */
        }

        /* Skip decisions (quirk 5). */
        if (!S_ISDIR(src_sb.st_mode)) {
            if (x->update == CHOPIN_UPDATE_NONE) {
                if (x->debug)
                    printf("skipped %s\n", chopin_quoteaf(dst_name));
                return true;
            }
            if (x->update == CHOPIN_UPDATE_NONE_FAIL) {
                chopin_error(0, "not replacing %s", chopin_quoteaf(dst_name));
                return false;
            }
            if (x->interactive == CHOPIN_I_ASK_USER
                && !overwrite_ok(x, dst_name, dst_dirfd, dst_relname,
                                 &dst_sb)) {
                if (x->debug)
                    printf("skipped %s\n", chopin_quoteaf(dst_name));
                return false;
            }
        }

        /* dir/non-dir mismatch (src-dir variant is sprint 06). */
        if (!S_ISDIR(src_sb.st_mode) && S_ISDIR(dst_sb.st_mode)) {
            chopin_error(0, "cannot overwrite directory %s with "
                            "non-directory %s",
                         chopin_quoteaf_n(0, dst_name),
                         chopin_quoteaf_n(1, src_name));
            return false;
        }

        /* dest_info clobber guard: sprint 03. Backup block: sprint 03
           (a -b invocation over an existing dst cannot proceed
           honestly yet). */
        if (x->backup_type != CHOPIN_BACKUP_NONE
            && !S_ISDIR(dst_sb.st_mode)) {
            chopin_error(0, "internal: backups arrive in sprint 03");
            return false;
        }

        /* Unlink-before (2.1 item 6 tail; the preserve_links and
           DEREF_NEVER arms complete in sprint 05). */
        if (!S_ISDIR(dst_sb.st_mode) && x->unlink_dest_before_opening) {
            if (unlinkat(dst_dirfd, dst_relname, 0) != 0) {
                chopin_error(errno, "cannot remove %s",
                             chopin_quoteaf(dst_name));
                return false;
            }
            if (x->verbose)
                printf("removed %s\n", chopin_quoteaf(dst_name));
            new_dst = true;
        }
    }

    /* Item 7: just-created-symlink guard - sprint 03 (dest_info). */

    /* Item 8: -v prints BEFORE copying (quirk 6). */
    if (x->verbose && !S_ISDIR(src_sb.st_mode))
        printf("%s -> %s\n", chopin_quoteaf_n(0, src_name),
               chopin_quoteaf_n(1, dst_name));

    /* Items 9-11: hard-link bookkeeping and earlier-file hits are
       sprints 05/06; -l/-s dispatch is sprint 05. */
    if (x->hard_link || x->symbolic_link) {
        chopin_error(0, "internal: -l/-s dispatch arrives in sprint 05");
        return false;
    }
    if (S_ISLNK(src_sb.st_mode)) {
        chopin_error(0, "internal: symlink copying arrives in sprint 05");
        return false;
    }

    /* Item 12: omitted_permissions - zero for regular files without
       preserve_ownership (sprint 04 completes the matrix). */
    mode_t dst_mode_bits = src_sb.st_mode & 07777;

    /* Item 14: type dispatch - regular (or copy_as_regular: special
       files read as data without -R, quirk 13). */
    if (S_ISREG(src_sb.st_mode)
        || (x->copy_as_regular && !S_ISLNK(src_sb.st_mode))) {
        if (!copy_reg(src_name, dst_name, dst_dirfd, dst_relname, x,
                      dst_mode_bits, &new_dst, &src_sb))
            return false;
    } else {
        chopin_error(0, "internal: special-file dispatch arrives in "
                        "sprint 06");
        return false;
    }

    /* Item 15: dest_info recording - sprint 03. Metadata tail -
       sprint 04. */
    return true;
}
