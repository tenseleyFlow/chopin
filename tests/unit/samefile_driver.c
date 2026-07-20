/* samefile_driver - the same_file_ok 10-case matrix, enumerated
 * directly against real fixtures (gnu-cp-analysis.md 2.3). Built by
 * tests/unit/run.sh and linked against the src objects. */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "copy.h"
#include "options.h"

static int failures;

static void
base_opts(struct chopin_options *x)
{
    memset(x, 0, sizeof *x);
    x->dereference = CHOPIN_DEREF_ALWAYS;
    x->backup_type = CHOPIN_BACKUP_NONE;
    x->update = CHOPIN_UPDATE_ALL;
    x->copy_as_regular = true;
    x->data_copy_required = true;
}

/* stat with the deref mode the spine would use. */
static void
xstat(const char *name, enum chopin_deref d, struct stat *st)
{
    int flags = d == CHOPIN_DEREF_NEVER ? AT_SYMLINK_NOFOLLOW : 0;

    if (fstatat(AT_FDCWD, name, st, flags) != 0) {
        perror(name);
        exit(2);
    }
}

static void
check(const char *name, const char *src, const char *dst,
      const struct chopin_options *x, bool want_ok, bool want_return_now)
{
    struct stat ssb, dsb;
    bool return_now = false, diagnosed = false;

    xstat(src, x->dereference, &ssb);
    xstat(dst, x->dereference, &dsb);
    bool ok = chopin_same_file_ok(src, &ssb, AT_FDCWD, dst, &dsb, x,
                                  &return_now, &diagnosed);
    if (ok != want_ok || return_now != want_return_now) {
        printf("FAIL %s: ok=%d/%d return_now=%d/%d\n", name,
               ok, want_ok, return_now, want_return_now);
        failures++;
    }
}

int
main(void)
{
    char tmpl[] = "/tmp/chopin-samefile.XXXXXX";
    struct chopin_options x;

    if (mkdtemp(tmpl) == NULL || chdir(tmpl) != 0) {
        perror("mkdtemp");
        return 2;
    }

    /* Fixtures: f; g = hardlink(f); h separate; s1,s2 symlinks to f;
       slh = hardlink of the symlink s1 itself. */
    int fd = open("f", O_WRONLY | O_CREAT, 0644);
    if (fd < 0 || write(fd, "x\n", 2) != 2 || close(fd) != 0)
        return 2;
    if (link("f", "g") != 0 || symlink("f", "s1") != 0
        || symlink("f", "s2") != 0
        || linkat(AT_FDCWD, "s1", AT_FDCWD, "slh", 0) != 0)
        return 2;
    fd = open("h", O_WRONLY | O_CREAT, 0644);
    if (fd < 0 || close(fd) != 0)
        return 2;

    /* 1: same inode + -l: OK, return-now (cp -l f f no-op). */
    base_opts(&x); x.hard_link = true;
    check("case1-l-noop", "f", "g", &x, true, true);
    check("case1-l-self", "f", "f", &x, true, true);

    /* 10 via 6: plain same-inode pairs are refused. */
    base_opts(&x);
    check("case6-self", "f", "f", &x, false, false);
    check("case6-hardlinks", "f", "g", &x, false, false);

    /* Distinct files trivially OK. */
    check("distinct", "f", "h", &x, true, false);

    /* 4: backups legalize hardlink pairs but not the same entry. */
    base_opts(&x); x.backup_type = CHOPIN_BACKUP_SIMPLE;
    check("case4-b-hardlink", "f", "g", &x, true, false);
    check("case4-b-self", "f", "f", &x, false, false);

    /* 4b: deref symlink src over its referent + backup would dangle
       the source. */
    check("case4-b-dangle", "s1", "f", &x, false, false);

    /* 2: DEREF_NEVER symlink pairs. */
    base_opts(&x); x.dereference = CHOPIN_DEREF_NEVER;
    check("case2-P-distinct", "s1", "s2", &x, true, false);
    check("case2-P-self", "s1", "s1", &x, false, false);
    check("case2-P-hardlinked", "s1", "slh", &x, true, true);

    /* 2 + backup: distinct symlink names always OK. */
    x.backup_type = CHOPIN_BACKUP_SIMPLE;
    check("case2-P-b-distinct", "s1", "s2", &x, true, false);

    /* 9: DEREF_NEVER final resolution catches symlink-onto-referent. */
    base_opts(&x); x.dereference = CHOPIN_DEREF_NEVER;
    check("case9-P-sym-onto-ref", "s1", "f", &x, false, false);
    x.hard_link = true;
    check("case9-P-hardlink", "s1", "f", &x, true, true);

    /* 8: -s recreating a destination symlink. */
    base_opts(&x); x.symbolic_link = true;
    check("case8-s-dst-symlink", "f", "s1", &x, true, false);

    /* 5: --remove-destination with a symlink dst. */
    base_opts(&x); x.unlink_dest_before_opening = true;
    check("case5-rd-symlink-dst", "f", "s1", &x, true, false);

    /* Cleanup. */
    (void)!chdir("/");
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", tmpl);
    (void)!system(cmd);

    if (failures) {
        printf("samefile_driver: %d FAILED\n", failures);
        return 1;
    }
    printf("samefile_driver: 16 cases ok\n");
    return 0;
}
