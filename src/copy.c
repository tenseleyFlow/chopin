#include "copy.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backup.h"
#include "config.h"
#include "copydata.h"
#include "meta.h"
#include "forcelink.h"
#include "hashes.h"
#include "parallel.h"
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

/* linkat(0 flags) links the symlink itself on Linux/macOS/FreeBSD
   (POSIX 2008 default). */
#define CHOPIN_CAN_HARDLINK_SYMLINKS 1

/* Recursion state (copy.c dir_list): the chain of source dirs above
   this point, for cycle detection. */
struct dir_list {
    struct dir_list *parent;
    ino_t st_ino;
    dev_t st_dev;
};

/* Into-itself diagnostics print the TOP-LEVEL names (quirk 19). */
static const char *top_level_src_name;
static const char *top_level_dst_name;

/* Primed by chopin_copy_init before any pool worker exists: lazy
   first-use initialization would be a data race under the pool. */
static int verify_enabled;
/* CHOPIN_DEBUG_WALK=1 (sprint 10): traverse sources and run the full
   decision ladder but mutate NOTHING - the spine-traversal gate
   times walk+plan alone. Meaningful for fresh-dst recursive copies
   (the gate's shape); creation, data, and metadata sites are
   skipped. */
static int walk_only;
static void chopin_copy_chunks_init(void);

void
chopin_copy_init(void)
{
    const char *e = getenv("CHOPIN_DEBUG_VERIFY");

    verify_enabled = e != NULL && *e != '\0' && *e != '0';
    e = getenv("CHOPIN_DEBUG_WALK");
    walk_only = e != NULL && *e != '\0' && *e != '0';
    chopin_copydata_init();
    chopin_copy_chunks_init();
    (void)chopin_cached_umask();
    (void)chopin_euid();
}

static bool copy_internal(const char *src_name, const char *dst_name,
                          int dst_dirfd, const char *dst_relname,
                          int nonexistent_dst,
                          const struct stat *parent_sb,
                          struct dir_list *ancestors,
                          const struct chopin_options *x,
                          bool command_line_arg,
                          bool *first_dir_created,
                          bool *copy_into_self);

static bool
is_ancestor(const struct stat *sb, const struct dir_list *ancestors)
{
    for (; ancestors != NULL; ancestors = ancestors->parent)
        if (ancestors->st_ino == sb->st_ino
            && ancestors->st_dev == sb->st_dev)
            return true;
    return false;
}

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

/* copy.c:1547-1570: link via the force protocol; verbose prints
   removed %s when an existing dest was atomically replaced. */
static bool
create_hard_link(const char *src_name,
                 int src_dirfd, const char *src_relname,
                 const char *dst_name,
                 int dst_dirfd, const char *dst_relname,
                 bool replace, bool verbose, bool dereference)
{
    int err = chopin_force_linkat(src_dirfd, src_relname,
                                  dst_dirfd, dst_relname,
                                  dereference ? AT_SYMLINK_FOLLOW : 0,
                                  replace);
    if (err > 0) {
        chopin_error(err, "cannot create hard link %s to %s",
                     chopin_quoteaf_n(0, dst_name),
                     chopin_quoteaf_n(1, src_name));
        return false;
    }
    if (err < 0 && verbose)
        printf("removed %s\n", chopin_quoteaf(dst_name));
    return true;
}

/* areadlink: readlink with the stat-size hint, growing on
   truncation. */
static char *
areadlink_with_size(const char *name, size_t hint)
{
    size_t cap = hint ? hint + 1 : 128;

    for (;;) {
        char *buf = chopin_xmalloc(cap);
        ssize_t n = readlink(name, buf, cap);

        if (n < 0) {
            int save = errno;
            free(buf);
            errno = save;
            return NULL;
        }
        if ((size_t)n < cap) {
            buf[n] = '\0';
            return buf;
        }
        free(buf);
        cap *= 2;
    }
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
    /* All earlier stderr must precede the prompt (sprint 09B). */
    chopin_parallel_flush_for_prompt();
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

/* --debug per-file line (copy-file-data.c emit_debug): byte-parity
   with GNU's vocabulary from sprint 07 on. */
static void
emit_debug(const struct chopin_options *x,
           const struct chopin_copy_debug *d)
{
    if (!x->debug || x->hard_link || x->symbolic_link
        || !x->data_copy_required)
        return;
    printf("copy offload: %s, reflink: %s, sparse detection: %s\n",
           d->offload, d->reflink, d->sparse);
}

/* copy_reg per 2.4; scalar engine until sprint 07. */
static bool
copy_reg(const char *src_name, const char *dst_name,
         int dst_dirfd, const char *dst_relname,
         const struct chopin_options *x, mode_t dst_mode,
         mode_t omitted_permissions, bool *new_dst,
         struct stat *src_sb)
{
    struct stat src_open_sb;
    struct stat sb;
    int source_desc;
    int dest_desc = -1;
    bool have_dest_sb_pre = false;
    struct stat dest_sb_pre;
    bool return_val = true;
    /* GNU reports stages never consulted as "unknown" (a successful
       clone leaves offload/sparse unknown); the data path downgrades
       its own stages to "no" on entry. */
    struct chopin_copy_debug debug = { "unknown", "unknown", "unknown" };
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

    mode_t open_mode = (dst_mode & ~omitted_permissions)
        | (x->preserve_xattr && chopin_euid() != 0 ? S_IWUSR : 0);
    mode_t extra_permissions = open_mode & ~dst_mode;

    if (*new_dst) {
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

    /* Dest fstat BEFORE the engine rungs (sprint 10B): the clone
       rung needs only st_dev, which this fstat already yields -
       re-fstating inside clone_file was a third stat per file on
       the cold-swarm profile. fstat is side-effect-free and clone
       never changes the fields the metadata tail consumes (mode/
       owner baseline), so the reorder is behavior-neutral. */
    if (fstat(dest_desc, &sb) != 0) {
        chopin_error(errno, "cannot fstat %s", chopin_quoteaf(dst_name));
        return_val = false;
        goto close_src_and_dst_desc;
    }
    if (!*new_dst) {
        dest_sb_pre = sb;
        have_dest_sb_pre = true;
    }

    /* Engine ladder rung 1: FICLONE (copy.c:1006-1011). Attempted for
       every regular copy under reflink!=never - except through
       chopin's per-(src_dev,dst_dev) failed-probe cache (overview
       s5): non-CoW pairs pay ONE failed ioctl per run, not one per
       file. REFLINK_ALWAYS bypasses the cache (each file must
       attempt). --debug still reports per-file vocabulary (the cache
       changes syscalls, not output). */
    bool data_done = false;

    if (x->data_copy_required
        && x->reflink_mode != CHOPIN_REFLINK_NEVER) {
        int clone_err = chopin_clone_file(dest_desc, source_desc,
                                          src_open_sb.st_dev,
                                          sb.st_dev, *new_dst, x);
        if (clone_err == 0) {
            debug.reflink = "yes";
            data_done = true;
        } else if (clone_err > 0) {
            /* handle_clone_fail (679-710): diagnose when ALWAYS or
               terminal; unlink a fresh dest; fatal iff ALWAYS or
               terminal. */
            bool terminal = clone_err == EIO || clone_err == ENOMEM
                || clone_err == ENOSPC || clone_err == EDQUOT;
            if (x->reflink_mode == CHOPIN_REFLINK_ALWAYS || terminal) {
                chopin_error(clone_err, "failed to clone %s from %s",
                             chopin_quoteaf_n(0, dst_name),
                             chopin_quoteaf_n(1, src_name));
                if (x->reflink_mode == CHOPIN_REFLINK_ALWAYS && *new_dst
                    && (!terminal
                        || lseek(dest_desc, 0, SEEK_END) == 0)) {
                    if (unlinkat(dst_dirfd, dst_relname, 0) != 0)
                        chopin_error(errno, "cannot remove %s",
                                     chopin_quoteaf(dst_name));
                }
                return_val = false;
                goto close_src_and_dst_desc;
            }
            debug.reflink = "unsupported";
        } else {
            /* Cache hit: probe skipped. */
            debug.reflink = "unsupported";
        }
    } else if (x->data_copy_required) {
        debug.reflink = "no";
    }

    /* xattr-on-readonly dance (2.4 item 8): widen an existing dest
       temporarily; if the chmod fails, drop extra_permissions. */
    if (extra_permissions && !*new_dst) {
        if (fchmod(dest_desc, sb.st_mode | extra_permissions) != 0)
            extra_permissions = 0;
    }

    if (x->data_copy_required && !data_done) {
        if (!chopin_copy_file_data(source_desc, &src_open_sb, src_name,
                                   dest_desc, &sb, dst_name, x, &debug))
            return_val = false;
    }

    /* Metadata tail in the load-bearing order (3.1). */
    if (!chopin_apply_meta_fd(source_desc, src_name, dest_desc, dst_name,
                              &src_open_sb,
                              have_dest_sb_pre ? &dest_sb_pre : NULL,
                              &sb, *new_dst, dst_mode,
                              omitted_permissions,
                              extra_permissions, x))
        return_val = false;

    /* CHOPIN_DEBUG_VERIFY v2 (the inverted oracle at full strength):
       re-fstat asserts size/mode; a full content re-read compares
       the destination against the source byte-for-byte after ANY
       engine, plus a sparseness-class sanity bound. */
    if (return_val) {
        if (verify_enabled) {
            struct stat vsb;

            if (fstat(dest_desc, &vsb) != 0) {
                chopin_error(errno, "VERIFY: cannot fstat %s",
                             chopin_quoteaf(dst_name));
                exit(CHOPIN_STATUS_FAIL);
            }
            if (x->data_copy_required && S_ISREG(src_open_sb.st_mode)
                && vsb.st_size != src_open_sb.st_size)
                chopin_die(0, "VERIFY: size mismatch on %s",
                           chopin_quoteaf(dst_name));
            if (x->preserve_mode) {
                mode_t got = vsb.st_mode & 07777;
                mode_t want = dst_mode;
                mode_t want_soft =
                    dst_mode & (mode_t)~(S_ISUID | S_ISGID | S_ISVTX);
                if (got != want && got != want_soft)
                    chopin_die(0, "VERIFY: mode %04o on %s",
                               (unsigned)got, chopin_quoteaf(dst_name));
            }
            /* v2: content re-read after ANY engine. dest_desc is
               write-only; re-open by name for reading. Buffers are
               heap-per-call: verify is a debug oracle and may run on
               any pool worker (no shared statics). */
            if (x->data_copy_required && S_ISREG(src_open_sb.st_mode)) {
                enum { VBUF = 65536 };
                char *vs = chopin_xmalloc(2 * VBUF);
                char *vd = vs + VBUF;
                int vfd = openat(dst_dirfd, dst_relname, O_RDONLY);

                if (vfd < 0 || lseek(source_desc, 0, SEEK_SET) != 0)
                    chopin_die(errno, "VERIFY: cannot reopen %s",
                               chopin_quoteaf(dst_name));
                for (;;) {
                    ssize_t ns = read(source_desc, vs, VBUF);
                    ssize_t nd = read(vfd, vd, VBUF);

                    if (ns < 0 || nd < 0)
                        chopin_die(errno, "VERIFY: re-read failed on %s",
                                   chopin_quoteaf(dst_name));
                    if (ns != nd || memcmp(vs, vd, (size_t)ns) != 0)
                        chopin_die(0, "VERIFY: content mismatch on %s",
                                   chopin_quoteaf(dst_name));
                    if (ns == 0)
                        break;
                }
                close(vfd);
                free(vs);
                /* Sparseness sanity: the dest never occupies more
                   blocks than data + fs slack when holes were asked
                   for on a sparse source. */
                if (x->sparse_mode == CHOPIN_SPARSE_ALWAYS
                    && (intmax_t)src_open_sb.st_blocks * 512
                       < src_open_sb.st_size
                    && (intmax_t)vsb.st_blocks * 512
                       > src_open_sb.st_size + (intmax_t)1048576)
                    chopin_die(0, "VERIFY: sparseness lost on %s",
                               chopin_quoteaf(dst_name));
            }
        }
    }

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

    /* GNU prints the --debug line for FAILED copies too (all-unknown
       when nothing was consulted) - fuzz trial 42-76. */
    emit_debug(x, &debug);
    return return_val;
}

/* ---- Parallel payload (sprint 09B) --------------------------------
   The dispatch unit is one regular-file payload: the WORKER does
   open/copy/close + metadata via the unchanged copy_reg (its
   diagnostics land in the slot through the thread capture); the
   spine pre-computed the decision and already emitted the -v line in
   traversal order. The join hook runs on the spine at replay time
   and performs the tail bookkeeping copy_internal would have done
   inline: forget_created on failure, dest_info backfill on success.

   The payload owns EVERYTHING it touches - strings and the options
   struct BY VALUE. The options otherwise live in copy_dir stack
   frames, and since sprint 10B's deferred barriers those frames can
   die while the payload is still in flight (the 0-byte-file UAF,
   identity trials 9001-9/11/12). */
struct reg_payload {
    char *src_name;
    char *dst_name;
    size_t rel_off;
    int dst_dirfd;
    struct chopin_options opts;
    mode_t dst_mode;
    mode_t omitted;
    bool new_dst;
    struct stat src_sb;
    bool record_dest;           /* cmdline && multi-source: item 15 */
    bool have_dst_sb_out;
    struct stat dst_sb_out;
};

static bool
reg_payload_run(void *arg)
{
    struct reg_payload *p = arg;
    bool ok = copy_reg(p->src_name, p->dst_name, p->dst_dirfd,
                       p->dst_name + p->rel_off, &p->opts, p->dst_mode,
                       p->omitted, &p->new_dst, &p->src_sb);

    /* dest_info dev/ino backfill: fstat the result by name NOFOLLOW,
       exactly the stat serial copy_internal does at its tail. */
    if (ok && p->record_dest
        && fstatat(p->dst_dirfd, p->dst_name + p->rel_off,
                   &p->dst_sb_out, AT_SYMLINK_NOFOLLOW) == 0)
        p->have_dst_sb_out = true;
    return ok;
}

static void
reg_payload_join(void *arg, bool ok)
{
    struct reg_payload *p = arg;

    if (!ok)
        chopin_forget_created(p->src_sb.st_dev, p->src_sb.st_ino);
    if (ok && p->record_dest && p->have_dst_sb_out)
        chopin_dest_record(p->dst_name + p->rel_off, &p->dst_sb_out);
    free(p->src_name);
    free(p->dst_name);
    free(p);
}

/* ---- Chunked large-file dispatch (sprint 09B, DEFAULT OFF) --------
   CHOPIN_PARALLEL_CHUNKS=1 enables; sprint 10 measures chunked vs
   serial copy_file_range on the large-single lane before any default
   flip. Files >= CHOPIN_CHUNK_THRESHOLD (default 64 MiB) on
   parallel-classed pairs split into disjoint-offset pread/pwrite
   payloads over one shared fd pair with a refcounted close (wcp's
   pattern): ftruncate-to-size up front, ftruncate-back to the lowest
   failed offset on failure so partial-failure trees stay GNU-shaped.
   Restricted to CLEAN CREATES of dense sources whose device pair is
   memoized clone-unsupported: no open-ladder or reflink subtleties
   ride along, and exactly one diagnostic (the lowest-offset failure)
   reaches the slot stream - identity with serial holds. The LAST
   chunk to finish runs the metadata tail, the verify oracle, and the
   dest_info backfill; joins are idempotent through the control
   block. */
struct chunk_ctl {
    int src_fd;
    int dst_fd;
    char *src_name;
    char *dst_name;
    size_t rel_off;
    int dst_dirfd;
    struct chopin_options opts;  /* by value - frame-lifetime UAF */
    mode_t dst_mode;
    mode_t omitted;
    struct stat src_sb;
    bool record_dest;
    _Atomic int refs;           /* chunks still running */
    pthread_mutex_t lock;       /* failure record */
    bool failed;
    off_t fail_off;             /* lowest failing offset */
    int fail_errno;
    bool fail_on_read;
    bool final_ok;              /* set by the finisher */
    bool joined;                /* first join does the bookkeeping */
    int join_refs;              /* frees at the last join */
    bool have_dst_sb_out;
    struct stat dst_sb_out;
};

struct chunk_payload {
    struct chunk_ctl *ctl;
    off_t off;
    off_t len;
};

static void
chunk_record_failure(struct chunk_ctl *ctl, off_t off, int err,
                     bool on_read)
{
    pthread_mutex_lock(&ctl->lock);
    if (!ctl->failed || off < ctl->fail_off) {
        ctl->failed = true;
        ctl->fail_off = off;
        ctl->fail_errno = err;
        ctl->fail_on_read = on_read;
    }
    pthread_mutex_unlock(&ctl->lock);
}

static bool
chunk_finish(struct chunk_ctl *ctl)
{
    const struct chopin_options *x = &ctl->opts;
    bool ok = true;

    if (ctl->failed) {
        /* One diagnostic, at the serial failure point. */
        if (ctl->fail_on_read)
            chopin_error(ctl->fail_errno, "error reading %s",
                         chopin_quoteaf(ctl->src_name));
        else
            chopin_error(ctl->fail_errno, "error writing %s",
                         chopin_quoteaf(ctl->dst_name));
        if (ftruncate(ctl->dst_fd, ctl->fail_off) != 0) { /* GNU-shaped */
            chopin_error(errno, "failed to extend %s",
                         chopin_quoteaf(ctl->dst_name));
        }
        ok = false;
    } else {
        if (!chopin_apply_meta_fd(ctl->src_fd, ctl->src_name,
                                  ctl->dst_fd, ctl->dst_name,
                                  &ctl->src_sb, NULL, NULL, true,
                                  ctl->dst_mode, ctl->omitted, 0, x))
            ok = false;
        if (ok && verify_enabled) {
            enum { VBUF = 65536 };
            char *vs = chopin_xmalloc(2 * VBUF);
            char *vd = vs + VBUF;
            off_t pos = 0;
            /* The dst fd is write-only; verify needs its own. */
            int vfd = openat(ctl->dst_dirfd,
                             ctl->dst_name + ctl->rel_off, O_RDONLY);

            if (vfd < 0)
                chopin_die(errno, "VERIFY: cannot reopen %s",
                           chopin_quoteaf(ctl->dst_name));
            for (;;) {
                ssize_t ns = pread(ctl->src_fd, vs, VBUF, pos);
                ssize_t nd = pread(vfd, vd, VBUF, pos);

                if (ns < 0 || nd < 0)
                    chopin_die(errno, "VERIFY: re-read failed on %s",
                               chopin_quoteaf(ctl->dst_name));
                if (ns != nd || memcmp(vs, vd, (size_t)ns) != 0)
                    chopin_die(0, "VERIFY: content mismatch on %s",
                               chopin_quoteaf(ctl->dst_name));
                if (ns == 0)
                    break;
                pos += ns;
            }
            close(vfd);
            free(vs);
        }
        if (ok && ctl->record_dest
            && fstatat(ctl->dst_dirfd, ctl->dst_name + ctl->rel_off,
                       &ctl->dst_sb_out, AT_SYMLINK_NOFOLLOW) == 0)
            ctl->have_dst_sb_out = true;
    }

    if (close(ctl->dst_fd) < 0) {
        chopin_error(errno, "failed to close %s",
                     chopin_quoteaf(ctl->dst_name));
        ok = false;
    }
    if (close(ctl->src_fd) < 0) {
        chopin_error(errno, "failed to close %s",
                     chopin_quoteaf(ctl->src_name));
        ok = false;
    }
    ctl->final_ok = ok;
    return ok;
}

static bool
chunk_payload_run(void *arg)
{
    struct chunk_payload *c = arg;
    struct chunk_ctl *ctl = c->ctl;
    enum { CBUF = 1 << 20 };
    char *cbuf = chopin_xmalloc(CBUF);
    off_t pos = c->off;
    off_t end = c->off + c->len;

    while (pos < end) {
        size_t want = (size_t)(end - pos) < (size_t)CBUF
            ? (size_t)(end - pos) : (size_t)CBUF;
        ssize_t n = pread(ctl->src_fd, cbuf, want, pos);

        if (n < 0) {
            chunk_record_failure(ctl, pos, errno, true);
            break;
        }
        if (n == 0)
            break;      /* shrank underneath us; not a chunk error */

        ssize_t written = 0;

        while (written < n) {
            ssize_t w = pwrite(ctl->dst_fd, cbuf + written,
                               (size_t)(n - written), pos + written);

            if (w < 0) {
                chunk_record_failure(ctl, pos + written, errno, false);
                goto out;
            }
            written += w;
        }
        pos += n;
    }
out:
    free(cbuf);
    if (atomic_fetch_sub(&ctl->refs, 1) == 1)
        return chunk_finish(ctl);   /* the finisher carries the verdict */
    return true;
}

static void
chunk_payload_join(void *arg, bool ok)
{
    struct chunk_payload *c = arg;
    struct chunk_ctl *ctl = c->ctl;

    (void)ok;
    if (!ctl->joined) {
        ctl->joined = true;
        if (!ctl->final_ok)
            chopin_forget_created(ctl->src_sb.st_dev, ctl->src_sb.st_ino);
        if (ctl->final_ok && ctl->record_dest && ctl->have_dst_sb_out)
            chopin_dest_record(ctl->dst_name + ctl->rel_off,
                               &ctl->dst_sb_out);
    }
    free(c);
    if (--ctl->join_refs == 0) {
        pthread_mutex_destroy(&ctl->lock);
        free(ctl->src_name);
        free(ctl->dst_name);
        free(ctl);
    }
}

static bool chunks_enabled_flag;
static off_t chunk_threshold = 64 << 20;

static void
chopin_copy_chunks_init(void)
{
    const char *e = getenv("CHOPIN_PARALLEL_CHUNKS");

    chunks_enabled_flag = e != NULL && *e != '\0' && *e != '0';
    e = getenv("CHOPIN_CHUNK_THRESHOLD");
    if (e != NULL && *e != '\0') {
        long long v = strtoll(e, NULL, 10);

        if (v >= 65536)
            chunk_threshold = (off_t)v;
    }
}

/* Try to dispatch src as disjoint-offset chunks. Returns true if the
   file was taken (payloads dispatched); false = use the normal path.
   Restricted to clean creates of dense regulars on
   clone-unsupported pairs. */
static bool
try_chunk_dispatch(const char *src_name, const char *dst_name,
                   int dst_dirfd, const char *dst_relname,
                   const struct chopin_options *x, mode_t dst_mode,
                   mode_t omitted, bool new_dst,
                   const struct stat *src_sb, bool record_dest)
{
    if (!chunks_enabled_flag || !new_dst
        || src_sb->st_size < chunk_threshold
        || x->sparse_mode == CHOPIN_SPARSE_ALWAYS
        || (intmax_t)src_sb->st_blocks * 512 < src_sb->st_size)
        return false;

    int sfd = open(src_name, O_RDONLY);

    if (sfd < 0)
        return false;   /* the normal path will diagnose */

    struct stat open_sb;
    struct stat dparent;
    int drc = dst_dirfd == AT_FDCWD
        ? stat(".", &dparent) : fstat(dst_dirfd, &dparent);

    if (fstat(sfd, &open_sb) != 0 || !SAME_INODE(*src_sb, open_sb)
        || drc != 0
        || (x->reflink_mode != CHOPIN_REFLINK_NEVER
            && !chopin_clone_pair_known_unsupported(open_sb.st_dev,
                                                    dparent.st_dev))) {
        close(sfd);
        return false;
    }

    mode_t open_mode = (dst_mode & ~omitted)
        | (x->preserve_xattr && chopin_euid() != 0 ? S_IWUSR : 0);
    int dfd = openat(dst_dirfd, dst_relname,
                     O_WRONLY | O_CREAT | O_EXCL, open_mode);

    if (dfd < 0) {
        close(sfd);
        return false;   /* dangling symlinks etc: the ladder handles */
    }
    if (ftruncate(dfd, src_sb->st_size) != 0) {
        close(dfd);
        close(sfd);
        unlinkat(dst_dirfd, dst_relname, 0);
        return false;
    }

    struct chunk_ctl *ctl = chopin_xmalloc(sizeof *ctl);

    memset(ctl, 0, sizeof *ctl);
    ctl->src_fd = sfd;
    ctl->dst_fd = dfd;
    ctl->src_name = chopin_xstrdup(src_name);
    ctl->dst_name = chopin_xstrdup(dst_name);
    ctl->rel_off = (size_t)(dst_relname - dst_name);
    ctl->dst_dirfd = dst_dirfd;
    ctl->opts = *x;
    ctl->dst_mode = dst_mode;
    ctl->omitted = omitted;
    ctl->src_sb = open_sb;
    ctl->record_dest = record_dest;
    pthread_mutex_init(&ctl->lock, NULL);

    off_t chunk = 16 << 20;
    int n = (int)((src_sb->st_size + chunk - 1) / chunk);

    if (n < 1)
        n = 1;
    if (n > 64)
        n = 64;
    chunk = (src_sb->st_size + n - 1) / n;
    atomic_store(&ctl->refs, n);
    ctl->join_refs = n;

    for (int i = 0; i < n; i++) {
        struct chunk_payload *c = chopin_xmalloc(sizeof *c);

        c->ctl = ctl;
        c->off = (off_t)i * chunk;
        c->len = c->off + chunk <= src_sb->st_size
            ? chunk : src_sb->st_size - c->off;
        chopin_parallel_dispatch(chunk_payload_run, chunk_payload_join,
                                 c, ctl->dst_name);
    }
    return true;
}

/* Eligibility per the locked decisions: only clean-create and
   clobber-without-ask payloads dispatch. Interactive modes, --update
   against an existing dest, backups, --debug (its stdout line is
   computed during the copy), -f -v clobbers (the "removed" print),
   and non-regular sources stay on the spine. */
static bool
dispatch_eligible(const struct chopin_options *x, const struct stat *src_sb,
                  bool have_dst_sb, char *dst_backup)
{
    if (!S_ISREG(src_sb->st_mode))
        return false;
    if (x->debug || !x->data_copy_required)
        return false;
    if (dst_backup != NULL)
        return false;
    /* Multi-link sources under --preserve=links stay serial: a
       dispatched primary forces every follower into a full drain
       (hardlink-farm measured 13% BEHIND GNU with primaries
       dispatched; link groups flow serially without drains). */
    if (x->preserve_links && src_sb->st_nlink > 1)
        return false;
    /* Empty files stay serial: the payload would be open/close/meta
       with zero data, and the pool roundtrip costs more than it
       carries (release-scale swarm-empty: parallel 11.55s vs serial
       11.38s over 1M empties). */
    if (src_sb->st_size == 0)
        return false;
    if (have_dst_sb
        && (x->update != CHOPIN_UPDATE_ALL
            || x->interactive == CHOPIN_I_ASK_USER
            || (x->verbose && x->unlink_dest_after_failed_open)))
        return false;
    return true;
}

bool
chopin_copy(const char *src_name, const char *dst_name,
            int dst_dirfd, const char *dst_relname,
            int nonexistent_dst, const struct chopin_options *x,
            bool *copy_into_self)
{
    bool first_dir_created = false;

    top_level_src_name = src_name;
    top_level_dst_name = dst_name;
    *copy_into_self = false;
    return copy_internal(src_name, dst_name, dst_dirfd, dst_relname,
                         nonexistent_dst, NULL, NULL, x, true,
                         &first_dir_created, copy_into_self);
}

/* copy_dir (copy.c:379-440): full name snapshot before copying,
   sorted by NAME - chopin's deterministic order; the harness compares
   order-insensitively against GNU's inode order (overview s2). -H
   demotes to DEREF_NEVER for children. */
static int
namecmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool
copy_dir(const char *src_name_in, const char *dst_name_in,
         int dst_dirfd, const char *dst_relname_in, bool new_dst,
         const struct stat *src_sb, struct dir_list *ancestors,
         const struct chopin_options *x,
         bool *first_dir_created_per_command_line_arg,
         bool *copy_into_self)
{
    struct chopin_options non_command_line_options = *x;
    bool ok = true;
    DIR *dirp = opendir(src_name_in);
    char **names = NULL;
    size_t count = 0, cap = 0, i;

    if (dirp == NULL) {
        chopin_error(errno, "cannot access %s", chopin_quoteaf(src_name_in));
        return false;
    }
    for (;;) {
        struct dirent *de;

        errno = 0;
        de = readdir(dirp);
        if (de == NULL) {
            if (errno != 0) {
                chopin_error(errno, "cannot access %s",
                             chopin_quoteaf(src_name_in));
                ok = false;
            }
            break;
        }
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 32;
            names = chopin_xrealloc(names, cap * sizeof *names);
        }
        names[count++] = chopin_xstrdup(de->d_name);
    }
    closedir(dirp);
    if (count > 0)
        qsort(names, count, sizeof *names, namecmp);

    /* Launch the advisory cache-warming cascade only for WIDE
       directories (sprint 10B): a tiny -R copy must not pay a
       16-thread pool spawn (the smallfile lane measured 2x GNU when
       it did). Wide dirs cover the swarm/kernel shapes; the cascade
       recurses from here on its own. Never in walk mode, whose gate
       times the bare spine. */
    if (count >= 32 && !walk_only)
        chopin_parallel_prefetch_tree(src_name_in);

    if (x->dereference == CHOPIN_DEREF_COMMAND_LINE_ARGUMENTS)
        non_command_line_options.dereference = CHOPIN_DEREF_NEVER;

    size_t srclen = strlen(src_name_in);
    size_t dstlen = strlen(dst_name_in);
    size_t reloff = (size_t)(dst_relname_in - dst_name_in);
    bool new_first_dir_created = false;

    for (i = 0; i < count; i++) {
        bool local_into_self = false;
        size_t nl = strlen(names[i]);
        char *csrc = chopin_xmalloc(srclen + 1 + nl + 1);
        char *cdst = chopin_xmalloc(dstlen + 1 + nl + 1);
        bool fdc = *first_dir_created_per_command_line_arg;

        memcpy(csrc, src_name_in, srclen);
        csrc[srclen] = '/';
        memcpy(csrc + srclen + 1, names[i], nl + 1);
        memcpy(cdst, dst_name_in, dstlen);
        cdst[dstlen] = '/';
        memcpy(cdst + dstlen + 1, names[i], nl + 1);
        ok &= copy_internal(csrc, cdst, dst_dirfd, cdst + reloff,
                            new_dst ? 1 : 0, src_sb, ancestors,
                            &non_command_line_options, false,
                            &fdc, &local_into_self);
        *copy_into_self |= local_into_self;
        free(csrc);
        free(cdst);
        free(names[i]);
        if (local_into_self) {
            while (++i < count)
                free(names[i]);
            break;
        }
        new_first_dir_created |= fdc;
    }
    free(names);
    *first_dir_created_per_command_line_arg = new_first_dir_created;
    return ok;
}

static bool
copy_internal(const char *src_name, const char *dst_name,
              int dst_dirfd, const char *dst_relname,
              int nonexistent_dst, const struct stat *parent_sb,
              struct dir_list *ancestors, const struct chopin_options *x,
              bool command_line_arg, bool *first_dir_created,
              bool *copy_into_self)
{
    struct stat src_sb;
    struct stat dst_sb;
    bool new_dst = false;
    bool have_dst_sb = false;

    *copy_into_self = false;

    /* Sprint 09: if either operand names a dest whose payload is
       still in flight, drain first - the ladder must observe the
       completed file exactly as serial execution would (multi-operand
       collisions, and reading a dest an earlier operand just wrote). */
    if (chopin_parallel_pending_path(src_name)
        || chopin_parallel_pending_path(dst_name))
        chopin_parallel_drain_keep();

    /* 2.1 item 2: stat source. */
    int fflags = should_dereference(x, command_line_arg)
        ? 0 : AT_SYMLINK_NOFOLLOW;
    if (fstatat(AT_FDCWD, src_name, &src_sb, fflags) != 0) {
        chopin_error(errno, "cannot stat %s", chopin_quoteaf(src_name));
        return false;
    }

    /* Item 3: directories need -R. */
    if (S_ISDIR(src_sb.st_mode) && !x->recursive) {
        chopin_error(0, "-r not specified; omitting directory %s",
                     chopin_quoteaf(src_name));
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
    bool use_lstat =
        (!S_ISREG(src_sb.st_mode)
         && (!x->copy_as_regular
             || S_ISDIR(src_sb.st_mode) || S_ISLNK(src_sb.st_mode))
         && !(S_ISDIR(src_sb.st_mode) && x->keep_directory_symlink))
        || x->symbolic_link || x->hard_link
        || x->backup_type != CHOPIN_BACKUP_NONE
        || x->unlink_dest_before_opening;

    if (nonexistent_dst > 0) {
        new_dst = true;     /* freshly created parent: nothing exists */
    } else if (nonexistent_dst < 0 && !use_lstat) {
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
#elif CHOPIN_HAVE_ST_MTIMESPEC
            bool older =
                dst_sb.st_mtimespec.tv_sec < src_sb.st_mtimespec.tv_sec
                || (dst_sb.st_mtimespec.tv_sec == src_sb.st_mtimespec.tv_sec
                    && dst_sb.st_mtimespec.tv_nsec
                       < src_sb.st_mtimespec.tv_nsec);
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

        /* dir/non-dir mismatch (copy.c:1880-1893) - only when NO
           backups: a backup renames the conflicting dest aside and
           the copy proceeds (fuzz trial 42-119). */
        if (x->backup_type == CHOPIN_BACKUP_NONE) {
            if (S_ISDIR(src_sb.st_mode) && !S_ISDIR(dst_sb.st_mode)) {
                chopin_error(0, "cannot overwrite non-directory %s with "
                                "directory %s",
                             chopin_quoteaf_n(0, dst_name),
                             chopin_quoteaf_n(1, src_name));
                return false;
            }
            if (!S_ISDIR(src_sb.st_mode) && S_ISDIR(dst_sb.st_mode)) {
                chopin_error(0, "cannot overwrite directory %s with "
                                "non-directory %s",
                             chopin_quoteaf_n(0, dst_name),
                             chopin_quoteaf_n(1, src_name));
                return false;
            }
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
        /* Unlink-before is ELSE-IF chained after the backup block in
           GNU (1966): a backed-up dest is already renamed away -
           unlinking its old name would ENOENT (fuzz 1234-45/99). */
        else if (!S_ISDIR(dst_sb.st_mode)
            && (x->unlink_dest_before_opening
                || (x->data_copy_required
                    && ((x->preserve_links && 1 < dst_sb.st_nlink)
                        || (x->dereference == CHOPIN_DEREF_NEVER
                            && !S_ISREG(src_sb.st_mode)))))) {
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
    bool dest_is_symlink = false;
    const char *earlier_file = NULL;

    goto after_labels;
tail_fail:
    ok = false;
    goto tail;
after_labels:

    /* Item 9 (copy.c:2052-2075): dirs record on the command line,
       look up below it; multi-link sources record under
       --preserve=links. */
    if (S_ISDIR(src_sb.st_mode)) {
        if (command_line_arg)
            earlier_file = chopin_remember_copied(dst_name, src_sb.st_dev,
                                                  src_sb.st_ino);
        else
            earlier_file = chopin_src_to_dest_lookup(src_sb.st_dev,
                                                     src_sb.st_ino);
    }
    if (!S_ISDIR(src_sb.st_mode)
        && x->preserve_links && !x->hard_link
        && (1 < src_sb.st_nlink
            || (command_line_arg
                && x->dereference == CHOPIN_DEREF_COMMAND_LINE_ARGUMENTS)
            || x->dereference == CHOPIN_DEREF_ALWAYS))
        earlier_file = chopin_remember_copied(dst_name, src_sb.st_dev,
                                              src_sb.st_ino);

    /* Hardlink follower whose primary payload is still in flight
       (sprint 09B): followers never dispatch - drain, then
       re-resolve. A failed primary was forgotten at its join, so the
       re-lookup inserts THIS file as the new primary and it takes
       the fresh-copy path, exactly GNU's failure behavior. */
    if (earlier_file != NULL
        && chopin_parallel_pending_path(earlier_file)) {
        chopin_parallel_drain_keep();
        earlier_file = chopin_remember_copied(dst_name, src_sb.st_dev,
                                              src_sb.st_ino);
    }

    /* Item 10 (2080-2143): earlier-file hits. Dir cases print the
       TOP-LEVEL names (quirk 19). */
    if (earlier_file != NULL && S_ISDIR(src_sb.st_mode)) {
        int sn = chopin_same_nameat(AT_FDCWD, src_name,
                                    AT_FDCWD, earlier_file);
        if (sn == 1) {
            chopin_error(0, "cannot copy a directory, %s, into itself, %s",
                         chopin_quoteaf_n(0, top_level_src_name),
                         chopin_quoteaf_n(1, top_level_dst_name));
            *copy_into_self = true;
            goto tail_fail;
        }
        int dn = chopin_same_nameat(AT_FDCWD, dst_name,
                                    AT_FDCWD, earlier_file);
        if (dn == 1) {
            chopin_error(0, "warning: source directory %s specified "
                            "more than once", chopin_quoteaf(src_name));
            free(dst_backup);
            return true;
        }
        if (x->dereference == CHOPIN_DEREF_ALWAYS
            || (x->dereference == CHOPIN_DEREF_COMMAND_LINE_ARGUMENTS
                && command_line_arg)) {
            /* -L/-H revisit: fall through silently. */
            earlier_file = NULL;
        } else {
            chopin_error(0, "will not create hard link %s to directory "
                            "%s", chopin_quoteaf_n(0, dst_name),
                         chopin_quoteaf_n(1, earlier_file));
            goto tail_fail;
        }
    }
    if (earlier_file != NULL) {
        if (!create_hard_link(earlier_file, AT_FDCWD, earlier_file,
                              dst_name, dst_dirfd, dst_relname,
                              true, x->verbose,
                              should_dereference(x, command_line_arg))) {
            free(dst_backup);
            return false;
        }
        free(dst_backup);
        return true;
    }

    /* Item 14: type dispatch. Directory branch first
       (copy.c:2308-2419). */
    if (S_ISDIR(src_sb.st_mode)) {
        struct dir_list dir;
        mode_t dst_mode_bits = src_sb.st_mode & 07777;
        mode_t omitted = x->preserve_ownership
            ? (dst_mode_bits & (S_IRWXG | S_IRWXO))
            : (dst_mode_bits & (S_IWGRP | S_IWOTH));
        bool restore_dst_mode = false;
        bool created = false;

        if (is_ancestor(&src_sb, ancestors)) {
            chopin_error(0, "cannot copy cyclic symbolic link %s",
                         chopin_quoteaf(src_name));
            goto tail_fail;
        }

        dir.parent = ancestors;
        dir.st_ino = src_sb.st_ino;
        dir.st_dev = src_sb.st_dev;

        if (walk_only) {
            struct dir_list *anc = &dir;
            bool wdc = *first_dir_created;
            bool wok = true;

            if (!(x->one_file_system && parent_sb != NULL
                  && parent_sb->st_dev != src_sb.st_dev))
                wok = copy_dir(src_name, dst_name, dst_dirfd,
                               dst_relname, true, &src_sb, anc, x,
                               &wdc, copy_into_self);
            ok = wok;
            goto tail;
        }
        if (new_dst || !S_ISDIR(dst_sb.st_mode)) {
            mode_t mode = dst_mode_bits & ~omitted;

            if (mkdirat(dst_dirfd, dst_relname, mode) != 0) {
                chopin_error(errno, "cannot create directory %s",
                             chopin_quoteaf(dst_name));
                goto tail_fail;
            }
            created = true;
            if (fstatat(dst_dirfd, dst_relname, &dst_sb,
                        AT_SYMLINK_NOFOLLOW) != 0) {
                chopin_error(errno, "cannot stat %s",
                             chopin_quoteaf(dst_name));
                goto tail_fail;
            }
            if ((dst_sb.st_mode & S_IRWXU) != S_IRWXU) {
                restore_dst_mode = true;
                if (fchmodat(dst_dirfd, dst_relname,
                             dst_sb.st_mode | S_IRWXU, 0) != 0) {
                    chopin_error(errno, "setting permissions for %s",
                                 chopin_quoteaf(dst_name));
                    goto tail_fail;
                }
            }
            /* First dir created per command-line arg: the
               cp -R dir dir detector (2369-2377). */
            if (!*first_dir_created) {
                chopin_remember_copied(dst_name, dst_sb.st_dev,
                                       dst_sb.st_ino);
                *first_dir_created = true;
            }
            if (x->verbose)
                printf("%s -> %s\n", chopin_quoteaf_n(0, src_name),
                       chopin_quoteaf_n(1, dst_name));
        } else {
            omitted = 0;
        }

        bool delayed_ok = true;
        if (x->one_file_system && parent_sb != NULL
            && parent_sb->st_dev != src_sb.st_dev) {
            /* -x prunes CONTENTS, not the top dir (quirk at
               2402-2407). */
        } else {
            delayed_ok = copy_dir(src_name, dst_name, dst_dirfd,
                                  dst_relname, created, &src_sb, &dir,
                                  x, first_dir_created, copy_into_self);
        }

        /* Per-directory join point (sprint 09B), made CONDITIONAL in
           sprint 10B: the barrier exists so the directory's own
           post-order metadata lands after its contents. When that
           metadata mutates nothing - plain -R, no preserve flags, no
           mode to restore or re-widen - the barrier is pure futex
           churn (36% of the warm kernel-tree profile across 4219
           dirs) and payloads may keep flowing; the dispatch-side
           slot cap and the final barrier still bound everything. */
        bool dir_meta_mutates =
            x->preserve_timestamps || x->preserve_ownership
            || x->preserve_mode || x->explicit_no_preserve_mode
            || x->preserve_xattr || restore_dst_mode
            || omitted != 0;
        if (dir_meta_mutates)
            delayed_ok &= chopin_parallel_barrier();

        /* Post-order: the directory's own metadata after its
           contents (3.1); rides the call stack, no side structure -
           the shape sprint 09's barriers adopt. */
        if (!chopin_apply_meta_dir(src_name, dst_dirfd, dst_relname,
                                   dst_name, &src_sb, created,
                                   dst_mode_bits, omitted,
                                   restore_dst_mode, x))
            delayed_ok = false;

        /* Content/metadata failures never un_backup in GNU (only the
           pre-dispatch gotos do): the copied-so-far dir stays, and
           renaming the file backup over a DIRECTORY would EISDIR
           anyway (fuzz 42-119). */
        if (!delayed_ok) {
            free(dst_backup);
            dst_backup = NULL;
        }
        ok = delayed_ok;
        goto tail;
    }
    if (x->symbolic_link) {
        dest_is_symlink = true;
        if (*src_name != '/') {
            /* Relative sources demand the dest in the CWD (quirk 4,
               quotef wording). */
            struct stat dot_sb, dst_parent_sb;
            const char *slash = strrchr(dst_relname, '/');
            char parent[4096];
            bool in_current_dir;

            if (slash == NULL)
                strcpy(parent, ".");
            else {
                size_t n = (size_t)(slash - dst_relname);
                if (n >= sizeof parent)
                    n = sizeof parent - 1;
                if (n == 0)
                    n = 1;      /* "/x" -> "/" */
                memcpy(parent, dst_relname,
                       n == 1 && dst_relname[0] == '/' ? 1 : n);
                parent[n] = '\0';
            }
            in_current_dir =
                (dst_dirfd == AT_FDCWD && strcmp(parent, ".") == 0)
                || stat(".", &dot_sb) != 0
                || fstatat(dst_dirfd, parent, &dst_parent_sb, 0) != 0
                || SAME_INODE(dot_sb, dst_parent_sb);
            if (!in_current_dir) {
                chopin_error(0, "%s: can make relative symbolic links "
                                "only in current directory",
                             chopin_quotef(dst_name));
                ok = false;
                goto tail;
            }
        }
        int err = chopin_force_symlinkat(src_name, dst_dirfd, dst_relname,
                                         x->unlink_dest_after_failed_open);
        if (err > 0) {
            chopin_error(err, "cannot create symbolic link %s to %s",
                         chopin_quoteaf_n(0, dst_name),
                         chopin_quoteaf_n(1, src_name));
            ok = false;
            goto tail;
        }
    } else if (x->hard_link
               && !(S_ISLNK(src_sb.st_mode)
                    && x->dereference == CHOPIN_DEREF_NEVER
                    && !CHOPIN_CAN_HARDLINK_SYMLINKS)) {
        bool replace = x->unlink_dest_after_failed_open
            || x->interactive == CHOPIN_I_ASK_USER;
        if (!create_hard_link(src_name, AT_FDCWD, src_name,
                              dst_name, dst_dirfd, dst_relname,
                              replace, false,
                              should_dereference(x, command_line_arg))) {
            ok = false;
            goto tail;
        }
    } else if (S_ISREG(src_sb.st_mode)
               || (x->copy_as_regular && !S_ISLNK(src_sb.st_mode))) {
        /* Item 12 (copy.c:2288-2297): preserving ownership narrows
           group/other at creation; the metadata tail re-widens
           through the umask. copy_as_regular: special files read as
           data without -R (quirk 13). */
        mode_t dst_mode_bits = src_sb.st_mode & 07777;
        mode_t omitted = x->preserve_ownership
            ? (dst_mode_bits & (S_IRWXG | S_IRWXO)) : 0;

        if (walk_only) {
            ok = true;
            goto tail;
        }
        if (dispatch_eligible(x, &src_sb, have_dst_sb, dst_backup)
            && (chopin_parallel_note_eligible(src_sb.st_size),
                chopin_parallel_active())
            && chopin_parallel_pair_ok(src_sb.st_dev, src_name)) {
            bool rec = command_line_arg && chopin_multi_source_active();

            if (try_chunk_dispatch(src_name, dst_name, dst_dirfd,
                                   dst_relname, x, dst_mode_bits,
                                   omitted, new_dst, &src_sb, rec)) {
                free(dst_backup);
                return true;
            }

            struct reg_payload *p = chopin_xmalloc(sizeof *p);

            p->src_name = chopin_xstrdup(src_name);
            p->dst_name = chopin_xstrdup(dst_name);
            p->rel_off = (size_t)(dst_relname - dst_name);
            p->dst_dirfd = dst_dirfd;
            p->opts = *x;
            p->dst_mode = dst_mode_bits;
            p->omitted = omitted;
            p->new_dst = new_dst;
            p->src_sb = src_sb;
            p->record_dest = rec;
            p->have_dst_sb_out = false;
            chopin_parallel_dispatch(reg_payload_run, reg_payload_join,
                                     p, p->dst_name);
            /* Provisional success; the barrier ANDs the real result
               into this batch's return and the join hook does the
               tail bookkeeping. Nothing below the dispatch point
               touches this file serially: no backup exists and
               dest_record moved into the join. */
            free(dst_backup);
            return true;
        }
        ok = copy_reg(src_name, dst_name, dst_dirfd, dst_relname, x,
                      dst_mode_bits, omitted, &new_dst, &src_sb);
    } else if (S_ISLNK(src_sb.st_mode)) {
        char *src_link_val =
            areadlink_with_size(src_name, (size_t)src_sb.st_size);

        dest_is_symlink = true;
        if (src_link_val == NULL) {
            chopin_error(errno, "cannot read symbolic link %s",
                         chopin_quoteaf(src_name));
            ok = false;
            goto tail;
        }
        int symlink_err = chopin_force_symlinkat(
            src_link_val, dst_dirfd, dst_relname,
            x->unlink_dest_after_failed_open);
        /* DEV-003, kept quirk: --update=older treats an identical
           existing dest symlink as success (copy.c:2541-2557; GNU's
           own comment doubts it). */
        if (symlink_err > 0 && x->update == CHOPIN_UPDATE_OLDER
            && !new_dst && have_dst_sb && S_ISLNK(dst_sb.st_mode)
            && (size_t)dst_sb.st_size == strlen(src_link_val)) {
            char dest_val[4096];
            ssize_t n = readlinkat(dst_dirfd, dst_relname, dest_val,
                                   sizeof dest_val - 1);
            if (n >= 0) {
                dest_val[n] = '\0';
                if (strcmp(dest_val, src_link_val) == 0)
                    symlink_err = 0;
            }
        }
        if (symlink_err > 0) {
            chopin_error(symlink_err, "cannot create symbolic link %s",
                         chopin_quoteaf(dst_name));
            free(src_link_val);
            ok = false;
            goto tail;
        }
        free(src_link_val);

        if (x->preserve_ownership) {
            if (fchownat(dst_dirfd, dst_relname, src_sb.st_uid,
                         src_sb.st_gid, AT_SYMLINK_NOFOLLOW) != 0
                && !chopin_chown_failure_ok()) {
                /* DEV-002: GNU prints dst_name UNQUOTED here
                   (copy.c:2579-2580); chopin quotes it. */
                chopin_error(errno, "failed to preserve ownership "
                                    "for %s", chopin_quoteaf(dst_name));
                if (x->require_preserve) {
                    ok = false;
                    goto tail;
                }
            }
        }
    } else if (S_ISFIFO(src_sb.st_mode)) {
        mode_t mode = src_sb.st_mode
            & (mode_t)~(x->preserve_ownership
                        ? (S_IRWXG | S_IRWXO) : 0);
        if (mknodat(dst_dirfd, dst_relname, mode, 0) != 0
            && mkfifoat(dst_dirfd, dst_relname,
                        mode & (mode_t)~S_IFIFO) != 0) {
            chopin_error(errno, "cannot create fifo %s",
                         chopin_quoteaf(dst_name));
            ok = false;
            goto tail;
        }
        if (!chopin_apply_meta_name(dst_dirfd, dst_relname, dst_name,
                                    &src_sb, true,
                                    src_sb.st_mode & 07777,
                                    x->preserve_ownership
                                        ? (src_sb.st_mode
                                           & (S_IRWXG | S_IRWXO)) : 0,
                                    x))
            ok = false;
    } else if (S_ISBLK(src_sb.st_mode) || S_ISCHR(src_sb.st_mode)
               || S_ISSOCK(src_sb.st_mode)) {
        mode_t mode = src_sb.st_mode
            & (mode_t)~(x->preserve_ownership
                        ? (S_IRWXG | S_IRWXO) : 0);
        if (mknodat(dst_dirfd, dst_relname, mode,
                    src_sb.st_rdev) != 0) {
            chopin_error(errno, "cannot create special file %s",
                         chopin_quoteaf(dst_name));
            ok = false;
            goto tail;
        }
        if (!chopin_apply_meta_name(dst_dirfd, dst_relname, dst_name,
                                    &src_sb, true,
                                    src_sb.st_mode & 07777,
                                    x->preserve_ownership
                                        ? (src_sb.st_mode
                                           & (S_IRWXG | S_IRWXO)) : 0,
                                    x))
            ok = false;
    } else {
        chopin_error(0, "%s has unknown file type",
                     chopin_quoteaf(src_name));
        ok = false;
    }

    /* Name-based metadata for symlink dests: timestamps with
       NOFOLLOW (3.2); ownership was in-branch; mode never applies
       to symlinks; symlink xattrs cannot carry user.* on Linux. */
    if (ok && dest_is_symlink && x->preserve_timestamps) {
        struct timespec ts[2];

#if CHOPIN_HAVE_ST_MTIM
        ts[0] = src_sb.st_atim;
        ts[1] = src_sb.st_mtim;
#elif CHOPIN_HAVE_ST_MTIMESPEC
        ts[0] = src_sb.st_atimespec;
        ts[1] = src_sb.st_mtimespec;
#else
        ts[0].tv_sec = src_sb.st_atime;
        ts[0].tv_nsec = 0;
        ts[1].tv_sec = src_sb.st_mtime;
        ts[1].tv_nsec = 0;
#endif
        if (utimensat(dst_dirfd, dst_relname, ts,
                      AT_SYMLINK_NOFOLLOW) != 0) {
            chopin_error(errno, "preserving times for %s",
                         chopin_quoteaf(dst_name));
            if (x->require_preserve)
                ok = false;
        }
    }

tail:
    /* Item 16 (un_backup, copy.c:2747-2773): a failed copy restores
       the backup over the dest; forget the src_to_dest entry unless
       the failure WAS the earlier-file link (copy.c:2760). */
    if (!ok && earlier_file == NULL)
        chopin_forget_created(src_sb.st_dev, src_sb.st_ino);
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
