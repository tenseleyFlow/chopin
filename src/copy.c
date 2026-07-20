#include "copy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backup.h"
#include "config.h"
#include "copydata.h"
#include "hashes.h"
#include "quote.h"
#include "util.h"

static const char *
last_component_of(const char *name)
{
    const char *base = name;
    const char *p;
    bool last_was_slash = false;

    while (*base == '/')
        base++;
    for (p = base; *p; p++) {
        if (*p == '/')
            last_was_slash = true;
        else if (last_was_slash) {
            base = p;
            last_was_slash = false;
        }
    }
    return base;
}

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

/* lib/same.c same_nameat: same basename AND same parent dir inode.
   Returns 1 same, 0 different, -1 = parent stat failed. GNU calls
   error(1,...) and aborts the whole run on that failure (quirk 21);
   chopin diagnoses `cannot stat %s` and fails only this file
   (DEV-004). */
int
chopin_same_nameat(int dirfd_a, const char *a, int dirfd_b, const char *b)
{
    const char *base_a = last_component_of(a);
    const char *base_b = last_component_of(b);

    if (strcmp(base_a, base_b) != 0)
        return 0;

    char dira[4096];
    char dirb[4096];
    size_t la = (size_t)(base_a - a);
    size_t lb = (size_t)(base_b - b);
    struct stat sa, sb2;

    if (la == 0)
        strcpy(dira, ".");
    else {
        memcpy(dira, a, la < sizeof dira ? la : sizeof dira - 1);
        dira[la < sizeof dira ? la : sizeof dira - 1] = '\0';
    }
    if (lb == 0)
        strcpy(dirb, ".");
    else {
        memcpy(dirb, b, lb < sizeof dirb ? lb : sizeof dirb - 1);
        dirb[lb < sizeof dirb ? lb : sizeof dirb - 1] = '\0';
    }
    if (fstatat(dirfd_a, dira, &sa, 0) != 0) {
        chopin_error(errno, "cannot stat %s", chopin_quoteaf(dira));
        return -1;
    }
    if (fstatat(dirfd_b, dirb, &sb2, 0) != 0) {
        chopin_error(errno, "cannot stat %s", chopin_quoteaf(dirb));
        return -1;
    }
    return SAME_INODE(sa, sb2) ? 1 : 0;
}

/* copy.c:1163-1399 ported structurally; *diagnosed set when
   same_nameat already reported a failure (DEV-004: the caller fails
   the file without the same-file message). move_mode arms retained
   as engine seams (always false in cp). */
bool
chopin_same_file_ok(const char *src_name, const struct stat *src_sb,
                    int dst_dirfd, const char *dst_relname,
                    const struct stat *dst_sb,
                    const struct chopin_options *x,
                    bool *return_now, bool *diagnosed)
{
    const struct stat *src_sb_link;
    const struct stat *dst_sb_link;
    struct stat tmp_dst_sb;
    struct stat tmp_src_sb;
    bool same_link;
    bool same = SAME_INODE(*src_sb, *dst_sb);
    int sn;

    *return_now = false;
    *diagnosed = false;

    if (same && x->hard_link) {
        *return_now = true;
        return true;
    }

    if (x->dereference == CHOPIN_DEREF_NEVER) {
        same_link = same;

        if (S_ISLNK(src_sb->st_mode) && S_ISLNK(dst_sb->st_mode)) {
            sn = chopin_same_nameat(AT_FDCWD, src_name,
                                    dst_dirfd, dst_relname);
            if (sn < 0) {
                *diagnosed = true;
                return false;
            }
            if (!sn) {
                if (x->backup_type != CHOPIN_BACKUP_NONE)
                    return true;
                if (same_link) {
                    *return_now = true;
                    return !x->move_mode;
                }
            }
            return !sn;
        }
        src_sb_link = src_sb;
        dst_sb_link = dst_sb;
    } else {
        if (!same)
            return true;

        if (fstatat(dst_dirfd, dst_relname, &tmp_dst_sb,
                    AT_SYMLINK_NOFOLLOW) != 0
            || lstat(src_name, &tmp_src_sb) != 0)
            return true;

        src_sb_link = &tmp_src_sb;
        dst_sb_link = &tmp_dst_sb;

        same_link = SAME_INODE(*src_sb_link, *dst_sb_link);

        if (S_ISLNK(src_sb_link->st_mode) && S_ISLNK(dst_sb_link->st_mode)
            && x->unlink_dest_before_opening)
            return true;
    }

    if (x->backup_type != CHOPIN_BACKUP_NONE) {
        if (!same_link) {
            /* Backing up dst would dangle a dereferenced symlink
               source. */
            if (!x->move_mode
                && x->dereference != CHOPIN_DEREF_NEVER
                && S_ISLNK(src_sb_link->st_mode)
                && !S_ISLNK(dst_sb_link->st_mode))
                return false;
            return true;
        }
        sn = chopin_same_nameat(AT_FDCWD, src_name, dst_dirfd, dst_relname);
        if (sn < 0) {
            *diagnosed = true;
            return false;
        }
        return !sn;
    }

    if (x->move_mode || x->unlink_dest_before_opening) {
        if (S_ISLNK(dst_sb_link->st_mode))
            return true;
        if (same_link && 1 < dst_sb_link->st_nlink) {
            sn = chopin_same_nameat(AT_FDCWD, src_name,
                                    dst_dirfd, dst_relname);
            if (sn < 0) {
                *diagnosed = true;
                return false;
            }
            if (!sn)
                return !x->move_mode;
        }
    }

    if (!S_ISLNK(src_sb_link->st_mode) && !S_ISLNK(dst_sb_link->st_mode)) {
        if (!SAME_INODE(*src_sb_link, *dst_sb_link))
            return true;
        if (x->hard_link) {
            *return_now = true;
            return true;
        }
    }

    /* mv-only symlink-onto-referent case (engine seam; move_mode is
       never true in cp). */

    if (x->symbolic_link && S_ISLNK(dst_sb_link->st_mode))
        return true;

    if (x->dereference == CHOPIN_DEREF_NEVER) {
        if (!S_ISLNK(src_sb_link->st_mode))
            tmp_src_sb = *src_sb_link;
        else if (stat(src_name, &tmp_src_sb) != 0)
            return true;

        if (!S_ISLNK(dst_sb_link->st_mode))
            tmp_dst_sb = *dst_sb_link;
        else if (fstatat(dst_dirfd, dst_relname, &tmp_dst_sb, 0) != 0)
            return true;

        if (!SAME_INODE(tmp_src_sb, tmp_dst_sb))
            return true;

        if (x->hard_link) {
            *return_now = !S_ISLNK(dst_sb_link->st_mode);
            return true;
        }
    }

    return false;
}

/* copy.c:1586-1605: refuse a backup that would rename the source
   itself (simple/existing suffix collision). */
static bool
source_is_dst_backup(const char *srcbase, const struct stat *src_st,
                     int dst_dirfd, const char *dst_relname)
{
    size_t srcbaselen = strlen(srcbase);
    const char *dstbase = last_component_of(dst_relname);
    size_t dstbaselen = strlen(dstbase);
    const char *suffix = chopin_simple_backup_suffix();
    size_t suffixlen = strlen(suffix);

    if (!(srcbaselen == dstbaselen + suffixlen
          && memcmp(srcbase, dstbase, dstbaselen) == 0
          && strcmp(srcbase + dstbaselen, suffix) == 0))
        return false;

    size_t bsize = strlen(dst_relname) + suffixlen + 1;
    char *dst_back = chopin_xmalloc(bsize);
    struct stat dst_back_sb;
    int st;

    snprintf(dst_back, bsize, "%s%s", dst_relname, suffix);
    st = fstatat(dst_dirfd, dst_back, &dst_back_sb, 0);
    free(dst_back);
    return st == 0 && SAME_INODE(*src_st, dst_back_sb);
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

    /* Item 4: src_info duplicate guard (2 <= n_files only;
       cp.c:1700-1712): non-dir + no backups + seen -> warn, succeed. */
    if (command_line_arg && chopin_multi_source_active()
        && !S_ISDIR(src_sb.st_mode)
        && x->backup_type == CHOPIN_BACKUP_NONE
        && chopin_src_seen_or_record(src_name, &src_sb)) {
        chopin_error(0, "warning: source file %s specified more than once",
                     chopin_quoteaf(src_name));
        return true;
    }

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

    char *dst_backup = NULL;

    /* Item 6: existing-dst decision ladder. */
    if (have_dst_sb) {
        if (x->update != CHOPIN_UPDATE_NONE
            && x->update != CHOPIN_UPDATE_NONE_FAIL) {
            bool return_now = false;
            bool diagnosed = false;

            if (!chopin_same_file_ok(src_name, &src_sb, dst_dirfd,
                                     dst_relname, &dst_sb, x,
                                     &return_now, &diagnosed)) {
                if (!diagnosed)
                    chopin_error(0, "%s and %s are the same file",
                                 chopin_quoteaf_n(0, src_name),
                                 chopin_quoteaf_n(1, dst_name));
                return false;
            }
            if (return_now)
                return true;
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

        /* dest_info clobber guard (copy.c:1895-1910): numbered
           backups bypass it. */
        if (!S_ISDIR(dst_sb.st_mode) && command_line_arg
            && x->backup_type != CHOPIN_BACKUP_NUMBERED
            && chopin_multi_source_active()
            && chopin_dest_seen(dst_relname, &dst_sb)) {
            chopin_error(0, "will not overwrite just-created %s with %s",
                         chopin_quoteaf_n(0, dst_name),
                         chopin_quoteaf_n(1, src_name));
            return false;
        }

        /* Backup block per copy.c:1912-1965. */
        const char *srcbase;
        if (x->backup_type != CHOPIN_BACKUP_NONE
            && !(strcmp(srcbase = last_component_of(src_name), ".") == 0
                 || strcmp(srcbase, "..") == 0)
            && (x->move_mode || !S_ISDIR(dst_sb.st_mode))) {
            if (x->backup_type != CHOPIN_BACKUP_NUMBERED
                && source_is_dst_backup(srcbase, &src_sb, dst_dirfd,
                                        dst_relname)) {
                /* DEV-001: GNU prints two spaces after the semicolon
                   (copy.c:1934-1935); chopin prints one. */
                chopin_error(0, "backing up %s might destroy source; "
                                "%s not copied",
                             chopin_quoteaf_n(0, dst_name),
                             chopin_quoteaf_n(1, src_name));
                return false;
            }
            char *tmp_backup = chopin_backup_file_rename(dst_dirfd,
                                                         dst_relname,
                                                         x->backup_type);
            if (tmp_backup) {
                /* Splice under the dest dir prefix for diagnostics. */
                size_t dirlen = strlen(dst_name) - strlen(dst_relname);
                dst_backup = chopin_xmalloc(dirlen + strlen(tmp_backup) + 1);
                memcpy(dst_backup, dst_name, dirlen);
                strcpy(dst_backup + dirlen, tmp_backup);
                free(tmp_backup);
            } else if (errno != ENOENT) {
                chopin_error(errno, "cannot backup %s",
                             chopin_quoteaf(dst_name));
                return false;
            }
            new_dst = true;
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

    /* Item 7: just-created-symlink guard - 03C (dest_info). */

    /* Item 8: -v prints BEFORE copying (quirk 6), with the backup
       annotation (emit_verbose, copy.c:1506-1514). */
    if (x->verbose && !S_ISDIR(src_sb.st_mode)) {
        printf("%s -> %s", chopin_quoteaf_n(0, src_name),
               chopin_quoteaf_n(1, dst_name));
        if (dst_backup)
            printf(" (backup: %s)", chopin_quoteaf(dst_backup));
        putchar('\n');
    }

    bool ok = true;

    /* Items 9-11: hard-link bookkeeping and earlier-file hits are
       sprints 05/06; -l/-s dispatch is sprint 05. */
    if (x->hard_link || x->symbolic_link) {
        chopin_error(0, "internal: -l/-s dispatch arrives in sprint 05");
        ok = false;
    } else if (S_ISLNK(src_sb.st_mode)) {
        chopin_error(0, "internal: symlink copying arrives in sprint 05");
        ok = false;
    } else if (S_ISREG(src_sb.st_mode)
               || (x->copy_as_regular && !S_ISLNK(src_sb.st_mode))) {
        /* Item 12: omitted_permissions - zero for regular files
           without preserve_ownership (sprint 04 completes the
           matrix). Item 14: regular dispatch (or copy_as_regular:
           special files read as data without -R, quirk 13). */
        mode_t dst_mode_bits = src_sb.st_mode & 07777;

        ok = copy_reg(src_name, dst_name, dst_dirfd, dst_relname, x,
                      dst_mode_bits, &new_dst, &src_sb);
    } else {
        chopin_error(0, "internal: special-file dispatch arrives in "
                        "sprint 06");
        ok = false;
    }

    /* Item 16 (un_backup, copy.c:2747-2773): a failed copy restores
       the backup over the dest. */
    if (!ok && dst_backup) {
        const char *relbackup = dst_backup
            + (strlen(dst_name) - strlen(dst_relname));
        if (renameat(dst_dirfd, relbackup, dst_dirfd, dst_relname) != 0) {
            chopin_error(errno, "cannot un-backup %s",
                         chopin_quoteaf(dst_name));
        } else if (x->verbose) {
            printf("%s -> %s (unbackup)\n",
                   chopin_quoteaf_n(0, dst_backup),
                   chopin_quoteaf_n(1, dst_name));
        }
    }
    free(dst_backup);

    /* Item 15: record dest_info via a fresh NOFOLLOW stat
       (copy.c:2611-2618). Metadata tail - sprint 04. */
    if (ok && command_line_arg && chopin_multi_source_active()) {
        struct stat sb;

        if (fstatat(dst_dirfd, dst_relname, &sb,
                    AT_SYMLINK_NOFOLLOW) == 0)
            chopin_dest_record(dst_relname, &sb);
    }
    return ok;
}
