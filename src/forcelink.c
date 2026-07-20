#include "forcelink.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

/* Port of src/force-link.c: atomic replacement via a CuXXXXXX temp
   in the destination's own directory - link (or symlink) to the
   temp, renameat over the dest, unlink the temp regardless (the
   temp and dest may already be the same hard link, making renameat
   a no-op). Returns -1 replaced, 0 fresh, positive errno failure. */

static char *
samedir_template(const char *dstname)
{
    const char *slash = strrchr(dstname, '/');
    size_t dirlen = slash ? (size_t)(slash - dstname) + 1 : 0;
    char *t = chopin_xmalloc(dirlen + sizeof "CuXXXXXX");

    memcpy(t, dstname, dirlen);
    memcpy(t + dirlen, "CuXXXXXX", sizeof "CuXXXXXX");
    return t;
}

/* try_tempname_len reduced: cycle the X's; the caller's operation is
   the existence probe (EEXIST retries). */
static int
try_tempname(char *tmpl, int (*op)(const char *, void *), void *arg)
{
    char *xs = tmpl + strlen(tmpl) - 6;
    static const char alnum[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned long seed = (unsigned long)getpid();
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        seed ^= (unsigned long)ts.tv_nsec ^ (unsigned long)ts.tv_sec;
    for (int attempt = 0; attempt < 100; attempt++) {
        unsigned long v = seed + (unsigned long)attempt * 7919UL;
        for (int i = 0; i < 6; i++) {
            xs[i] = alnum[v % 62];
            v /= 62;
            v ^= v << 13;
        }
        if (op(tmpl, arg) == 0)
            return 0;
        if (errno != EEXIST)
            return -1;
    }
    errno = EEXIST;
    return -1;
}

struct link_arg {
    int srcdir;
    const char *srcname;
    int dstdir;
    int flags;
};

static int
try_link(const char *dest, void *varg)
{
    struct link_arg *a = varg;
    return linkat(a->srcdir, a->srcname, a->dstdir, dest, a->flags);
}

struct symlink_arg {
    const char *target;
    int dstdir;
};

static int
try_symlink(const char *dest, void *varg)
{
    struct symlink_arg *a = varg;
    return symlinkat(a->target, a->dstdir, dest);
}

static int
force_via_temp(const char *dstname, int dstdir,
               int (*op)(const char *, void *), void *arg)
{
    char *tmp = samedir_template(dstname);
    int err;

    if (try_tempname(tmp, op, arg) != 0) {
        err = errno;
    } else {
        err = renameat(dstdir, tmp, dstdir, dstname) == 0 ? -1 : errno;
        unlinkat(dstdir, tmp, 0);
    }
    free(tmp);
    return err;
}

int
chopin_force_linkat(int srcdir, const char *srcname,
                    int dstdir, const char *dstname, int flags, bool force)
{
    int err = linkat(srcdir, srcname, dstdir, dstname, flags) == 0
        ? 0 : errno;

    if (!force || err != EEXIST)
        return err;
    struct link_arg arg = { srcdir, srcname, dstdir, flags };
    return force_via_temp(dstname, dstdir, try_link, &arg);
}

int
chopin_force_symlinkat(const char *target, int dstdir,
                       const char *dstname, bool force)
{
    int err = symlinkat(target, dstdir, dstname) == 0 ? 0 : errno;

    if (!force || err != EEXIST)
        return err;
    struct symlink_arg arg = { target, dstdir };
    return force_via_temp(dstname, dstdir, try_symlink, &arg);
}
