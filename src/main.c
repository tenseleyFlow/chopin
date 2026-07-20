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
        /* dest_info_init/src_info_init for 2 <= n_files: sprint 03. */
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
                /* make_dir_parents_private + re_protect: sprint 06. */
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

                while (*rel == '/')
                    rel++;
                ok &= chopin_copy(arg, dst_name, target_dirfd, rel, 0,
                                  &inv->x, &into_self);
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

            if (samefile_rewrite) {
                /* cp.c:848-873: dest becomes the backup name and
                   backup_type clears for the actual copy - the name
                   must be generated BEFORE clearing. */
                dest = chopin_find_backup_name(AT_FDCWD, dest,
                                               inv->x.backup_type);
                x_local = inv->x;
                x_local.backup_type = CHOPIN_BACKUP_NONE;
                xp = &x_local;
            }
            ok = chopin_copy(source, dest, AT_FDCWD, dest, -new_dst,
                             xp, &into_self);
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

    const char *dbg = getenv("CHOPIN_DEBUG_OPTIONS");
    bool debug_options = dbg != NULL && *dbg != '\0' && *dbg != '0';

    do_copy(&inv, debug_options);
}
