#include "backup.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "util.h"

/* lib/backupfile.c port: suffix rules, simple/existing/numbered name
   generation with the all-9s string growth, NAME_MAX truncation,
   RENAME_NOREPLACE race loop. */

static const char *simple_suffix = "~";

void
chopin_set_simple_backup_suffix(const char *arg)
{
    if (arg == NULL)
        arg = getenv("SIMPLE_BACKUP_SUFFIX");
    /* Valid only if nonempty and slash-free (backupfile.c:77-88). */
    if (arg != NULL && *arg != '\0' && strchr(arg, '/') == NULL)
        simple_suffix = arg;
}

const char *
chopin_simple_backup_suffix(void)
{
    return simple_suffix;
}

/* Split FILE into (dir-part prefix length, base). */
static size_t
base_offset_of(const char *file)
{
    const char *slash = strrchr(file, '/');
    return slash ? (size_t)(slash - file) + 1 : 0;
}

static long
dir_name_max(int dirfd, const char *file, size_t base_off)
{
    char dirbuf[4096];
    long nm;

    if (base_off == 0) {
        nm = dirfd == AT_FDCWD ? pathconf(".", _PC_NAME_MAX)
                               : fpathconf(dirfd, _PC_NAME_MAX);
    } else {
        size_t n = base_off < sizeof dirbuf ? base_off : sizeof dirbuf - 1;
        memcpy(dirbuf, file, n);
        dirbuf[n] = '\0';
        if (dirfd == AT_FDCWD) {
            nm = pathconf(dirbuf, _PC_NAME_MAX);
        } else {
            int dfd = openat(dirfd, dirbuf, O_RDONLY | O_DIRECTORY);
            if (dfd < 0)
                return 255;
            nm = fpathconf(dfd, _PC_NAME_MAX);
            close(dfd);
        }
    }
    return nm > 0 ? nm : 255;
}

/* Numbered scan: find max N among FILE.~N~ siblings; return the next
   version as a malloc'd full name, and whether any numbered backup
   exists. String arithmetic only (all-9s grows by prepending). Scan
   errors degrade to N=1 (GNU: use the highest number found). */
static char *
numbered_backup_name(int dirfd, const char *file, bool *numbered_exists)
{
    size_t base_off = base_offset_of(file);
    const char *base = file + base_off;
    size_t baselen = strlen(base);
    char *version = chopin_xstrdup("1");
    DIR *dirp = NULL;
    int dfd = -1;

    *numbered_exists = false;

    if (base_off == 0) {
        dfd = openat(dirfd, ".", O_RDONLY | O_DIRECTORY);
    } else {
        char *dir = chopin_xmalloc(base_off + 1);
        memcpy(dir, file, base_off);
        dir[base_off] = '\0';
        dfd = openat(dirfd, dir, O_RDONLY | O_DIRECTORY);
        free(dir);
    }
    if (dfd >= 0)
        dirp = fdopendir(dfd);
    if (dirp == NULL) {
        if (dfd >= 0)
            close(dfd);
    } else {
        struct dirent *dp;

        while ((dp = readdir(dirp)) != NULL) {
            const char *n = dp->d_name;
            size_t nl = strlen(n);

            if (nl < baselen + 4 || memcmp(n, base, baselen) != 0
                || n[baselen] != '.' || n[baselen + 1] != '~')
                continue;
            const char *p = n + baselen + 2;
            if (!('1' <= *p && *p <= '9'))
                continue;
            bool all_9s = (*p == '9');
            size_t vl;
            for (vl = 1; p[vl] >= '0' && p[vl] <= '9'; vl++)
                all_9s = all_9s && p[vl] == '9';
            if (p[vl] != '~' || p[vl + 1] != '\0')
                continue;
            size_t curlen = strlen(version);
            if (!(curlen < vl
                  || (curlen == vl && memcmp(version, p, vl) <= 0)))
                continue;

            *numbered_exists = true;
            /* version = p + 1, string arithmetic. */
            free(version);
            version = chopin_xmalloc(vl + 2);
            if (all_9s) {
                version[0] = '0';
                memcpy(version + 1, p, vl);
                version[vl + 1] = '\0';
            } else {
                memcpy(version, p, vl);
                version[vl] = '\0';
            }
            char *q = version + strlen(version);
            while (*--q == '9')
                *q = '0';
            ++*q;
        }
        closedir(dirp);
    }

    size_t flen = strlen(file);
    size_t nsize = flen + 2 + strlen(version) + 2;
    char *name = chopin_xmalloc(nsize);
    snprintf(name, nsize, "%s.~%s~", file, version);
    free(version);
    return name;
}

/* check_extension reduced (backupfile.c:100-167): when base+suffix
   exceeds the dir's NAME_MAX, fall back to the "~" suffix; truncate
   the base as a last resort. */
static char *
apply_suffix_checked(int dirfd, const char *file, const char *suffix)
{
    size_t base_off = base_offset_of(file);
    size_t baselen = strlen(file + base_off);
    size_t sufflen = strlen(suffix);
    long name_max = dir_name_max(dirfd, file, base_off);
    char *name;

    if (baselen + sufflen > (size_t)name_max) {
        suffix = "~";
        sufflen = 1;
        if (baselen + sufflen > (size_t)name_max)
            baselen = (size_t)name_max - 1;
    }
    name = chopin_xmalloc(base_off + baselen + sufflen + 1);
    memcpy(name, file, base_off + baselen);
    memcpy(name + base_off + baselen, suffix, sufflen + 1);
    return name;
}

char *
chopin_find_backup_name(int dirfd, const char *file,
                        enum chopin_backup type)
{
    bool numbered_exists = false;

    if (type == CHOPIN_BACKUP_NUMBERED)
        return numbered_backup_name(dirfd, file, &numbered_exists);
    if (type == CHOPIN_BACKUP_NUMBERED_EXISTING) {
        char *name = numbered_backup_name(dirfd, file, &numbered_exists);
        if (numbered_exists)
            return name;
        free(name);
    }
    return apply_suffix_checked(dirfd, file, simple_suffix);
}

char *
chopin_backup_file_rename(int dirfd, const char *file,
                          enum chopin_backup type)
{
    for (;;) {
        char *name = chopin_find_backup_name(dirfd, file, type);
        bool numbered = type == CHOPIN_BACKUP_NUMBERED
            || (type == CHOPIN_BACKUP_NUMBERED_EXISTING
                && strlen(name) > strlen(file)
                && name[strlen(file)] == '.');
        int rc;

        if (numbered) {
#if CHOPIN_HAVE_RENAMEAT2
            rc = renameat2(dirfd, file, dirfd, name, RENAME_NOREPLACE);
#elif CHOPIN_HAVE_RENAMEATX_NP
            rc = renameatx_np(dirfd, file, dirfd, name, RENAME_EXCL);
#else
            /* No atomic no-replace rename: racy link+unlink fallback. */
            rc = linkat(dirfd, file, dirfd, name, 0);
            if (rc == 0)
                rc = unlinkat(dirfd, file, 0);
#endif
            if (rc != 0 && errno == EEXIST) {
                free(name);
                continue;   /* race: another backup appeared; re-scan */
            }
        } else {
            rc = renameat(dirfd, file, dirfd, name);
        }
        if (rc != 0) {
            free(name);
            return NULL;
        }
        return name;
    }
}

bool
chopin_un_backup(int dirfd, const char *backup, const char *file)
{
    return renameat(dirfd, backup, dirfd, file) == 0;
}
