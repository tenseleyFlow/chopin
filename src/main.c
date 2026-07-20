/* chopin - from-scratch C11 reimplementation of GNU cp(1).
 * main owns operand/target semantics (do_copy shape, gnu-cp-analysis.md
 * 1.5) and will own the copy_internal spine from sprint 02. cpn is the
 * same binary; argv[0] changes nothing but the diagnostic program name.
 *
 * Sprint 01: full option surface + resolution + operand/target
 * validation with GNU-exact diagnostics. No copying yet: valid copy
 * shapes refuse with exit 1, or dump and exit 0 under
 * CHOPIN_DEBUG_OPTIONS=1 (the unit-driver surface). */

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "backup.h"
#include "copy.h"
#include "hashes.h"
#include "meta.h"
#include "options.h"
#include "plan.h"
#include "quote.h"
#include "util.h"

/* gnulib close_stdout semantics (cp registers it atexit): a stdout
   write failure surfaces as `write error` and forces exit 1 even
   after a successful copy (gnu-cp-analysis.md 4.1). */
static void
close_stdout_atexit(void)
{
    bool prev_fail = ferror(stdout) != 0;
    int e = 0;

    if (fflush(stdout) != 0)
        e = errno;
    if (prev_fail || e != 0) {
        if (e != 0)
            chopin_error(e, "write error");
        else
            chopin_error(0, "write error");
        _exit(CHOPIN_STATUS_FAIL);
    }
}

#ifdef O_PATH
#define CHOPIN_O_PATHSEARCH O_PATH
#define CHOPIN_HAVE_O_PATH 1
#elif defined O_SEARCH
#define CHOPIN_O_PATHSEARCH O_SEARCH
#define CHOPIN_HAVE_O_PATH 0
#else
#define CHOPIN_O_PATHSEARCH O_RDONLY
#define CHOPIN_HAVE_O_PATH 0
#endif

static bool
target_dirfd_valid(int fd)
{
    return fd >= 0 || fd == AT_FDCWD;
}

/* lib/targetdir.c:38-52: true for ".", "./.", ".///./", etc. */
static bool
must_be_working_directory(const char *f)
{
    while (*f++ == '.') {
        if (*f != '/')
            return !*f;
        while (*++f == '/')
            continue;
        if (!*f)
            return true;
    }
    return false;
}

/* lib/targetdir.c:60-77 incl. the EACCES->ENOTDIR preference on
   platforms lacking O_PATH. */
static int
target_directory_operand(const char *file, struct stat *st)
{
    if (must_be_working_directory(file))
        return AT_FDCWD;

    int fd = open(file, CHOPIN_O_PATHSEARCH | O_DIRECTORY);

#if !CHOPIN_HAVE_O_PATH
    if (fd < 0 && errno == EACCES)
        errno = stat(file, st) < 0 || S_ISDIR(st->st_mode)
            ? EACCES : ENOTDIR;
#else
    (void)st;
#endif

    return fd - (AT_FDCWD == -1 && fd < 0);
}

/* gnulib last_component. */
static const char *
last_component(const char *name)
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

static void
strip_trailing_slashes_inplace(char *name)
{
    const char *base = last_component(name);
    char *end = name + strlen(name);

    /* Never strip a leading "/" down to nothing. */
    if (*base == '\0')
        end = name + (name[0] == '/' ? 1 : 0);
    else {
        while (end > base && end[-1] == '/')
            end--;
    }
    if (*end != '\0' && (end != name || name[0] != '\0'))
        *end = '\0';
}

/* gnulib mfile_name_concat: DIR + separator-if-needed + BASE;
   *base_in_result points at BASE's copy inside the result. */
static char *
file_name_concat(const char *dir, const char *base, char **base_in_result)
{
    size_t dirlen = strlen(dir);
    const char *dirbase = last_component(dir);
    size_t dirbaselen = strlen(dirbase);
    bool needs_sep = dirbaselen != 0 && dirbase[dirbaselen - 1] != '/';
    size_t baselen = strlen(base);
    char *p = chopin_xmalloc(dirlen + (needs_sep ? 1 : 0) + baselen + 1);
    char *q = p;

    memcpy(q, dir, dirlen);
    q += dirlen;
    if (needs_sep)
        *q++ = '/';
    if (base_in_result)
        *base_in_result = q;
    memcpy(q, base, baselen + 1);
    return p;
}

/* --parents machinery (cp.c:464-664, 370-442): pre-create missing
   intermediates recording source attrs, fix them up post-copy in
   times -> ownership -> mode order. */
struct dir_attr {
    struct stat st;
    size_t slash_offset;
    bool restore_mode;
    struct dir_attr *next;
};

static bool
make_dir_parents_private(const char *const_dir, size_t src_offset,
                         int dst_dirfd, bool verbose,
                         struct dir_attr **attr_list, bool *new_dst,
                         const struct chopin_options *x)
{
    const char *lastc = last_component(const_dir);
    size_t dirlen = (size_t)(lastc - const_dir);

    while (dirlen > src_offset && const_dir[dirlen - 1] == '/')
        dirlen--;
    *attr_list = NULL;
    if (dirlen <= src_offset)
        return true;

    char *dir = chopin_xstrdup(const_dir);
    char *src = dir + src_offset;
    char *dst_dir = chopin_xmalloc(dirlen + 1);
    memcpy(dst_dir, dir, dirlen);
    dst_dir[dirlen] = '\0';
    const char *dst_reldir = dst_dir + src_offset;
    while (*dst_reldir == '/')
        dst_reldir++;

    struct stat stats;
    bool ok = true;

    if (fstatat(dst_dirfd, dst_reldir, &stats, 0) != 0) {
        char *slash = src;

        while (*slash == '/')
            slash++;
        dst_reldir = slash;

        while ((slash = strchr(slash, '/')) != NULL) {
            struct dir_attr *new_attr = NULL;

            *slash = '\0';
            bool missing_dir =
                fstatat(dst_dirfd, dst_reldir, &stats, 0) != 0;

            if (missing_dir || x->preserve_ownership || x->preserve_mode
                || x->preserve_timestamps) {
                struct stat src_st;
                int src_errno = stat(src, &src_st) != 0 ? errno
                    : S_ISDIR(src_st.st_mode) ? 0 : ENOTDIR;

                if (src_errno != 0) {
                    chopin_error(src_errno, "failed to get attributes "
                                            "of %s", chopin_quoteaf(src));
                    ok = false;
                    goto out;
                }
                new_attr = chopin_xmalloc(sizeof *new_attr);
                new_attr->st = src_st;
                new_attr->slash_offset = (size_t)(slash - dir);
                new_attr->restore_mode = false;
                new_attr->next = *attr_list;
                *attr_list = new_attr;
            }

            if (missing_dir) {
                *new_dst = true;
                mode_t src_mode = new_attr->st.st_mode;
                mode_t omitted = src_mode
                    & (x->preserve_ownership ? (S_IRWXG | S_IRWXO)
                       : x->preserve_mode ? (S_IWGRP | S_IWOTH) : 0);
                mode_t mkdir_mode = (x->explicit_no_preserve_mode
                                     ? 0777 : src_mode)
                    & 07777 & (mode_t)~omitted;

                if (mkdirat(dst_dirfd, dst_reldir, mkdir_mode) != 0) {
                    chopin_error(errno, "cannot make directory %s",
                                 chopin_quoteaf(dir));
                    ok = false;
                    goto out;
                }
                if (verbose)
                    printf("%s -> %s\n", src, dir);
                if (fstatat(dst_dirfd, dst_reldir, &stats,
                            AT_SYMLINK_NOFOLLOW) != 0) {
                    chopin_error(errno, "failed to get attributes of %s",
                                 chopin_quoteaf(dir));
                    ok = false;
                    goto out;
                }
                if (!x->preserve_mode) {
                    if (omitted & ~stats.st_mode)
                        omitted &= (mode_t)~chopin_cached_umask();
                    if ((omitted & ~stats.st_mode) != 0
                        || (stats.st_mode & S_IRWXU) != S_IRWXU) {
                        new_attr->st.st_mode = stats.st_mode | omitted;
                        new_attr->restore_mode = true;
                    }
                }
                mode_t accessible = stats.st_mode | S_IRWXU;
                if (stats.st_mode != accessible
                    && fchmodat(dst_dirfd, dst_reldir, accessible,
                                0) != 0) {
                    chopin_error(errno, "setting permissions for %s",
                                 chopin_quoteaf(dir));
                    ok = false;
                    goto out;
                }
            } else if (!S_ISDIR(stats.st_mode)) {
                chopin_error(0, "%s exists but is not a directory",
                             chopin_quoteaf(dir));
                ok = false;
                goto out;
            } else {
                *new_dst = false;
            }
            *slash++ = '/';
            while (*slash == '/')
                slash++;
        }
    } else if (!S_ISDIR(stats.st_mode)) {
        chopin_error(0, "%s exists but is not a directory",
                     chopin_quoteaf(dst_dir));
        ok = false;
    } else {
        *new_dst = false;
    }
out:
    free(dst_dir);
    free(dir);
    return ok;
}

static bool
re_protect(const char *const_dst_name, size_t src_offset, int dst_dirfd,
           struct dir_attr *attr_list, const struct chopin_options *x)
{
    char *dst_name = chopin_xstrdup(const_dst_name);
    const char *relname = dst_name + src_offset;
    bool ok = true;

    while (*relname == '/')
        relname++;
    for (struct dir_attr *p = attr_list; p != NULL; p = p->next) {
        dst_name[p->slash_offset] = '\0';

        if (x->preserve_timestamps) {
            struct timespec ts[2];
#if CHOPIN_HAVE_ST_MTIM
            ts[0] = p->st.st_atim;
            ts[1] = p->st.st_mtim;
#elif CHOPIN_HAVE_ST_MTIMESPEC
            ts[0] = p->st.st_atimespec;
            ts[1] = p->st.st_mtimespec;
#else
            ts[0].tv_sec = p->st.st_atime;
            ts[0].tv_nsec = 0;
            ts[1].tv_sec = p->st.st_mtime;
            ts[1].tv_nsec = 0;
#endif
            if (utimensat(dst_dirfd, relname, ts, 0) != 0) {
                chopin_error(errno, "failed to preserve times for %s",
                             chopin_quoteaf(dst_name));
                ok = false;
                break;
            }
        }
        if (x->preserve_ownership) {
            if (fchownat(dst_dirfd, relname, p->st.st_uid, p->st.st_gid,
                         AT_SYMLINK_NOFOLLOW) != 0) {
                if (!chopin_chown_failure_ok()) {
                    chopin_error(errno, "failed to preserve ownership "
                                        "for %s", chopin_quoteaf(dst_name));
                    ok = false;
                    break;
                }
                (void)!fchownat(dst_dirfd, relname, (uid_t)-1,
                                p->st.st_gid, AT_SYMLINK_NOFOLLOW);
            }
        }
        if (x->preserve_mode || p->restore_mode) {
            if (fchmodat(dst_dirfd, relname, p->st.st_mode & 07777,
                         0) != 0) {
                chopin_error(errno, "failed to preserve permissions "
                                    "for %s", chopin_quoteaf(dst_name));
                ok = false;
                break;
            }
        }
        dst_name[p->slash_offset] = '/';
    }
    free(dst_name);
    return ok;
}

/* Sprint 01 stand-in for the copy engine: record the resolved pair.
   Sprint 02 replaces this with copy_internal. */
struct pair {
    char *src;
    char *dst;
};
static struct pair *pairs;
static int n_pairs;

static void
record_pair(const char *src, const char *dst)
{
    pairs = chopin_xrealloc(pairs, (size_t)(n_pairs + 1) * sizeof *pairs);
    pairs[n_pairs].src = chopin_xstrdup(src);
    pairs[n_pairs].dst = chopin_xstrdup(dst);
    n_pairs++;
}

static _Noreturn void
finish(const struct chopin_invocation *inv, bool debug_options,
       bool new_dst, bool ok)
{
    struct chopin_plan plan;

    chopin_plan_init(&plan, &inv->x, new_dst);
    chopin_plan_maybe_debug(&plan);

    if (debug_options) {
        chopin_options_dump(inv, stdout);
        for (int i = 0; i < n_pairs; i++)
            printf("map %s => %s\n", pairs[i].src, pairs[i].dst);
        exit(CHOPIN_STATUS_OK);
    }
    exit(ok ? CHOPIN_STATUS_OK : CHOPIN_STATUS_FAIL);
}

/* do_copy front half per cp.c:669-880. */
static _Noreturn void
do_copy(struct chopin_invocation *inv, bool debug_options)
{
    int n_files = inv->n_files;
    char **file = inv->files;
    const char *target_directory = inv->target_directory;
    bool no_target_directory = inv->no_target_directory;
    struct stat sb;
    int target_dirfd = AT_FDCWD;
    bool new_dst = false;

    if (n_files <= (target_directory ? 0 : 1)) {
        if (n_files <= 0)
            chopin_error(0, "missing file operand");
        else
            chopin_error(0, "missing destination file operand after %s",
                         chopin_quoteaf(file[0]));
        chopin_try_help_and_die();
    }

    sb.st_mode = 0;

    if (no_target_directory) {
        if (target_directory)
            chopin_die(0, "cannot combine --target-directory (-t) "
                          "and --no-target-directory (-T)");
        if (2 < n_files) {
            chopin_error(0, "extra operand %s", chopin_quoteaf(file[2]));
            chopin_try_help_and_die();
        }
    } else if (target_directory) {
        target_dirfd = target_directory_operand(target_directory, &sb);
        if (!target_dirfd_valid(target_dirfd))
            chopin_die(errno, "target directory %s",
                       chopin_quoteaf(target_directory));
    } else {
        const char *lastfile = file[n_files - 1];
        int fd = target_directory_operand(lastfile, &sb);
        if (target_dirfd_valid(fd)) {
            target_dirfd = fd;
            target_directory = lastfile;
            n_files--;
        } else {
            int err = errno;
            if (err == ENOENT)
                new_dst = true;

            /* On platforms lacking O_PATH the EACCES-on-directory
               case must still be fatal (cp.c:719-736). */
            if (2 < n_files
#if !CHOPIN_HAVE_O_PATH
                || (err == EACCES
                    && (sb.st_mode || stat(lastfile, &sb) == 0)
                    && S_ISDIR(sb.st_mode))
#endif
                )
                chopin_die(err, "target %s", chopin_quoteaf(lastfile));
        }
    }

    bool ok = true;

    if (target_directory) {
        /* Guard tables only when they can matter (cp.c:746-753): the
           cp-a-a-b and same-source-twice guards are OFF for
           single-source copies. */
        if (2 <= n_files && !debug_options)
            chopin_multi_source_init();
        for (int i = 0; i < n_files; i++) {
            char *dst_name;
            char *arg_in_concat;
            char *arg = file[i];

            /* Trailing slashes are meaningful only in source names. */
            if (inv->remove_trailing_slashes)
                strip_trailing_slashes_inplace(arg);

            if (inv->parents_option) {
                char *no_slash = chopin_xstrdup(arg);
                strip_trailing_slashes_inplace(no_slash);
                dst_name = file_name_concat(target_directory, no_slash,
                                            &arg_in_concat);
                free(no_slash);
            } else {
                char *arg_base = chopin_xstrdup(last_component(arg));
                strip_trailing_slashes_inplace(arg_base);
                /* 'cp -R source/.. dest' must not copy into 'dest/..'
                   (quirk 10). */
                dst_name = file_name_concat(target_directory,
                                            strcmp(arg_base, "..") == 0
                                                ? "" : arg_base,
                                            &arg_in_concat);
                free(arg_base);
            }
            if (debug_options) {
                record_pair(arg, dst_name);
            } else {
                const char *rel = arg_in_concat;
                bool into_self;
                bool parent_exists = true;
                bool pnew_dst = false;
                struct dir_attr *attr_list = NULL;
                size_t src_off = (size_t)(arg_in_concat - dst_name);

                while (*rel == '/')
                    rel++;
                if (inv->parents_option)
                    parent_exists = make_dir_parents_private(
                        dst_name, src_off, target_dirfd,
                        inv->x.verbose, &attr_list, &pnew_dst, &inv->x);
                if (!parent_exists) {
                    ok = false;
                } else {
                    ok &= chopin_copy(arg, dst_name, target_dirfd, rel,
                                      pnew_dst ? 1 : 0, &inv->x,
                                      &into_self);
                    if (inv->parents_option)
                        ok &= re_protect(dst_name, src_off,
                                         target_dirfd, attr_list,
                                         &inv->x);
                }
                while (attr_list != NULL) {
                    struct dir_attr *next = attr_list->next;
                    free(attr_list);
                    attr_list = next;
                }
            }
            free(dst_name);
        }
        finish(inv, debug_options, new_dst, ok);
    } else {
        if (inv->parents_option) {
            chopin_error(0, "with --parents, the destination must be a "
                            "directory");
            chopin_try_help_and_die();
        }

        /* 'cp --force --backup foo foo' rewrites dest to the backup
           name and clears backup_type for the actual copy
           (cp.c:848-873). Name generation is sprint 03; the resolved
           condition is recorded now so the dump pins it. */
        const char *source = file[0];
        const char *dest = file[1];
        bool samefile_rewrite =
            inv->x.unlink_dest_after_failed_open
            && inv->x.backup_type != CHOPIN_BACKUP_NONE
            && strcmp(source, dest) == 0
            && !new_dst
            && (sb.st_mode != 0 || stat(dest, &sb) == 0)
            && S_ISREG(sb.st_mode);

        if (debug_options) {
            record_pair(source, dest);
            if (samefile_rewrite)
                printf("samefile_backup_rewrite=1\n");
        } else {
            bool into_self;
            struct chopin_options x_local;
            const struct chopin_options *xp = &inv->x;
            char *rewritten = NULL;

            if (samefile_rewrite) {
                /* cp.c:848-873: dest becomes the backup name and
                   backup_type clears for the actual copy - the name
                   must be generated BEFORE clearing. */
                dest = rewritten = chopin_find_backup_name(
                    AT_FDCWD, dest, inv->x.backup_type);
                x_local = inv->x;
                x_local.backup_type = CHOPIN_BACKUP_NONE;
                xp = &x_local;
            }
            ok = chopin_copy(source, dest, AT_FDCWD, dest, -new_dst,
                             xp, &into_self);
            free(rewritten);
        }
        finish(inv, debug_options, new_dst, ok);
    }
}

int
main(int argc, char **argv)
{
    static struct chopin_invocation inv;

    setlocale(LC_ALL, "");
    chopin_set_program(argc > 0 && argv[0] ? argv[0] : "chopin");
    chopin_diag_init();
    atexit(close_stdout_atexit);

    chopin_parse_args(argc, argv, &inv);
    chopin_options_resolve(&inv);
    chopin_copy_init();

    const char *dbg = getenv("CHOPIN_DEBUG_OPTIONS");
    bool debug_options = dbg != NULL && *dbg != '\0' && *dbg != '0';

    do_copy(&inv, debug_options);
}
