/* manifest - deterministic result-tree fingerprint for the chopin harness.
 *
 * Usage: manifest [-t] [-x] ROOT
 *
 * Walks ROOT depth-first with entries sorted by name (byte order) and
 * emits one line per entry, sorted by path (overview 6.1; the locked
 * line format lives in sprint 00):
 *
 *   PATH TYPE MODE UID:GID SIZE HASH LINKCLASS TARGET SPARSECLASS
 *   [MTIME] [XATTRS]
 *
 *   PATH        tree-relative path (the sort key)
 *   TYPE        f d l p s b c or ?
 *   MODE        %04o of st_mode & 07777
 *   UID:GID     numeric
 *   SIZE        st_size for f; MAJ:MIN device numbers for b/c; else -
 *   HASH        FNV-1a 64 of content for f; else -
 *   LINKCLASS   first-seen path of the dev/ino class; - if nlink==1
 *               (regular files only; directories always -)
 *   TARGET      symlink target for l; else -
 *   SPARSECLASS S if st_blocks*512 < st_size (f only); else -
 *   MTIME       (-t only) seconds.nanoseconds for f/d/l
 *   XATTRS      (-x only) sorted user.* name=FNV64(value) pairs
 *               comma-joined; - if none or unsupported
 *
 * PATH, TARGET and LINKCLASS percent-encode bytes <= 0x20, 0x7F and '%'
 * so hostile names (fuzz tier) stay one-line-per-entry. Never follows
 * symlinks. Exit 1 on any walk/read error (a manifest must be total).
 *
 * Compiled by the harness, not part of the shipping binary.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"

#if CHOPIN_HAVE_GETXATTR
#include <sys/xattr.h>
#endif

static const char *progname = "manifest";
static int opt_times = 0;
static int opt_xattrs = 0;
static int exit_status = 0;

static void
warnp(const char *path, const char *what)
{
    fprintf(stderr, "%s: %s: %s: %s\n", progname, what, path,
            strerror(errno));
    exit_status = 1;
}

/* --- FNV-1a 64 ---------------------------------------------------- */

#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME 1099511628211ULL

static uint64_t
fnv1a(const unsigned char *buf, size_t n, uint64_t h)
{
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= buf[i];
        h *= FNV_PRIME;
    }
    return h;
}

/* --- dev/ino -> first-seen path table ------------------------------ */

struct linkent {
    dev_t dev;
    ino_t ino;
    char *path;
};

static struct linkent *links;
static size_t links_len;
static size_t links_cap;

static const char *
linkclass(dev_t dev, ino_t ino, const char *path)
{
    size_t i;

    for (i = 0; i < links_len; i++)
        if (links[i].dev == dev && links[i].ino == ino)
            return links[i].path;
    if (links_len == links_cap) {
        links_cap = links_cap ? links_cap * 2 : 64;
        links = realloc(links, links_cap * sizeof *links);
        if (links == NULL) {
            fprintf(stderr, "%s: out of memory\n", progname);
            exit(1);
        }
    }
    links[links_len].dev = dev;
    links[links_len].ino = ino;
    links[links_len].path = strdup(path);
    if (links[links_len].path == NULL) {
        fprintf(stderr, "%s: out of memory\n", progname);
        exit(1);
    }
    links_len++;
    return NULL;
}

/* --- output helpers ------------------------------------------------ */

static void
emit_escaped(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    for (; *p != '\0'; p++) {
        if (*p <= 0x20 || *p == 0x7F || *p == '%')
            printf("%%%02X", *p);
        else
            putchar(*p);
    }
}

static char
type_char(mode_t m)
{
    if (S_ISREG(m))
        return 'f';
    if (S_ISDIR(m))
        return 'd';
    if (S_ISLNK(m))
        return 'l';
    if (S_ISFIFO(m))
        return 'p';
    if (S_ISSOCK(m))
        return 's';
    if (S_ISBLK(m))
        return 'b';
    if (S_ISCHR(m))
        return 'c';
    return '?';
}

static int
hash_file(const char *full, uint64_t *out)
{
    unsigned char buf[65536];
    ssize_t n;
    uint64_t h = FNV_OFFSET;
    int fd = open(full, O_RDONLY | O_NOFOLLOW);

    if (fd < 0)
        return -1;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        h = fnv1a(buf, (size_t)n, h);
    if (n < 0) {
        int save = errno;
        close(fd);
        errno = save;
        return -1;
    }
    close(fd);
    *out = h;
    return 0;
}

/* --- xattr digest (-x) --------------------------------------------- */

#if CHOPIN_HAVE_GETXATTR
static int
xattr_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}
#endif

static void
emit_xattrs(const char *full)
{
#if CHOPIN_HAVE_GETXATTR
    char names[65536];
    char value[65536];
    char *sorted[1024];
    size_t count = 0, i;
    ssize_t len, vlen;
    char *p;
    int emitted = 0;

    len = llistxattr(full, names, sizeof names);
    if (len < 0) {
        if (errno == ENOTSUP || errno == EOPNOTSUPP) {
            fputs(" -", stdout);
            return;
        }
        warnp(full, "llistxattr");
        fputs(" -", stdout);
        return;
    }
    for (p = names; p < names + len; p += strlen(p) + 1) {
        if (strncmp(p, "user.", 5) == 0 && count < 1024)
            sorted[count++] = p;
    }
    if (count == 0) {
        fputs(" -", stdout);
        return;
    }
    qsort(sorted, count, sizeof *sorted, xattr_cmp);
    putchar(' ');
    for (i = 0; i < count; i++) {
        vlen = lgetxattr(full, sorted[i], value, sizeof value);
        if (vlen < 0) {
            warnp(full, "lgetxattr");
            continue;
        }
        if (emitted)
            putchar(',');
        emit_escaped(sorted[i]);
        printf("=%016" PRIx64,
               fnv1a((unsigned char *)value, (size_t)vlen, FNV_OFFSET));
        emitted = 1;
    }
    if (!emitted)
        putchar('-');
#else
    (void)full;
    fputs(" -", stdout);
#endif
}

/* --- walk ---------------------------------------------------------- */

static int
name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void walk(const char *root, const char *rel);

static void
emit_entry(const char *root, const char *rel)
{
    char full[4096];
    struct stat st;
    char t;

    if ((size_t)snprintf(full, sizeof full, "%s/%s", root, rel)
        >= sizeof full) {
        fprintf(stderr, "%s: path too long: %s\n", progname, rel);
        exit_status = 1;
        return;
    }
    if (lstat(full, &st) != 0) {
        warnp(rel, "lstat");
        return;
    }
    t = type_char(st.st_mode);

    emit_escaped(rel);
    printf(" %c %04o %ju:%ju", t, (unsigned)(st.st_mode & 07777),
           (uintmax_t)st.st_uid, (uintmax_t)st.st_gid);

    /* SIZE */
    if (t == 'f')
        printf(" %jd", (intmax_t)st.st_size);
    else if (t == 'b' || t == 'c')
        printf(" %ju:%ju", (uintmax_t)(st.st_rdev >> 8),
               (uintmax_t)(st.st_rdev & 0xFF));
    else
        fputs(" -", stdout);

    /* HASH. EACCES is deterministic seed content (permission-trap
       fixtures both sides share), printed as "!" without failing -
       the manifest stays total. Other read errors are fatal. */
    if (t == 'f') {
        uint64_t h;
        if (hash_file(full, &h) != 0) {
            if (errno != EACCES)
                warnp(rel, "read");
            fputs(" !", stdout);
        } else {
            printf(" %016" PRIx64, h);
        }
    } else {
        fputs(" -", stdout);
    }

    /* LINKCLASS */
    if (t == 'f' && st.st_nlink > 1) {
        const char *first = linkclass(st.st_dev, st.st_ino, rel);
        if (first != NULL) {
            putchar(' ');
            emit_escaped(first);
        } else {
            putchar(' ');
            emit_escaped(rel);
        }
    } else {
        fputs(" -", stdout);
    }

    /* TARGET */
    if (t == 'l') {
        char target[4096];
        ssize_t n = readlink(full, target, sizeof target - 1);
        if (n < 0) {
            warnp(rel, "readlink");
            fputs(" !", stdout);
        } else {
            target[n] = '\0';
            putchar(' ');
            emit_escaped(target);
        }
    } else {
        fputs(" -", stdout);
    }

    /* SPARSECLASS */
    if (t == 'f' && (intmax_t)st.st_blocks * 512 < (intmax_t)st.st_size)
        fputs(" S", stdout);
    else
        fputs(" -", stdout);

    /* MTIME (-t) */
    if (opt_times) {
        if (t == 'f' || t == 'd' || t == 'l') {
#if CHOPIN_HAVE_ST_MTIM
            printf(" %jd.%09ld", (intmax_t)st.st_mtim.tv_sec,
                   st.st_mtim.tv_nsec);
#elif CHOPIN_HAVE_ST_MTIMESPEC
            printf(" %jd.%09ld", (intmax_t)st.st_mtimespec.tv_sec,
                   st.st_mtimespec.tv_nsec);
#else
            printf(" %jd.000000000", (intmax_t)st.st_mtime);
#endif
        } else {
            fputs(" -", stdout);
        }
    }

    /* XATTRS (-x) */
    if (opt_xattrs) {
        if (t == 'f' || t == 'd')
            emit_xattrs(full);
        else
            fputs(" -", stdout);
    }

    putchar('\n');

    if (t == 'd')
        walk(root, rel);
}

static void
walk(const char *root, const char *rel)
{
    char full[4096];
    DIR *d;
    struct dirent *de;
    char **names = NULL;
    size_t count = 0, cap = 0, i;

    if (rel[0] != '\0')
        snprintf(full, sizeof full, "%s/%s", root, rel);
    else
        snprintf(full, sizeof full, "%s", root);

    d = opendir(full);
    if (d == NULL) {
        warnp(rel[0] ? rel : ".", "opendir");
        return;
    }
    while (errno = 0, (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 32;
            names = realloc(names, cap * sizeof *names);
            if (names == NULL) {
                fprintf(stderr, "%s: out of memory\n", progname);
                exit(1);
            }
        }
        names[count] = strdup(de->d_name);
        if (names[count] == NULL) {
            fprintf(stderr, "%s: out of memory\n", progname);
            exit(1);
        }
        count++;
    }
    if (errno != 0)
        warnp(rel[0] ? rel : ".", "readdir");
    closedir(d);

    qsort(names, count, sizeof *names, name_cmp);

    for (i = 0; i < count; i++) {
        char childrel[4096];
        if (rel[0] != '\0')
            snprintf(childrel, sizeof childrel, "%s/%s", rel, names[i]);
        else
            snprintf(childrel, sizeof childrel, "%s", names[i]);
        emit_entry(root, childrel);
        free(names[i]);
    }
    free(names);
}

int
main(int argc, char **argv)
{
    int i = 1;
    struct stat st;

    if (argc > 0 && argv[0] != NULL)
        progname = argv[0];
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        if (strcmp(argv[i], "-t") == 0)
            opt_times = 1;
        else if (strcmp(argv[i], "-x") == 0)
            opt_xattrs = 1;
        else {
            fprintf(stderr, "usage: %s [-t] [-x] ROOT\n", progname);
            return 1;
        }
    }
    if (i != argc - 1) {
        fprintf(stderr, "usage: %s [-t] [-x] ROOT\n", progname);
        return 1;
    }
    if (lstat(argv[i], &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s: %s is not a directory\n", progname, argv[i]);
        return 1;
    }

    walk(argv[i], "");

    if (fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "%s: write error\n", progname);
        return 1;
    }
    return exit_status;
}
