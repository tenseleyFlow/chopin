#include "hashes.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>

#include "copy.h"
#include "util.h"

struct triple {
    char *name;
    dev_t dev;
    ino_t ino;
};

struct table {
    struct triple *items;
    size_t len;
    size_t cap;
};

static bool active;
static struct table src_info;
static struct table dest_info;

void
chopin_multi_source_init(void)
{
    active = true;
}

bool
chopin_multi_source_active(void)
{
    return active;
}

static void
table_add(struct table *t, const char *name, const struct stat *sb)
{
    if (t->len == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 32;
        t->items = chopin_xrealloc(t->items, t->cap * sizeof *t->items);
    }
    t->items[t->len].name = chopin_xstrdup(name);
    t->items[t->len].dev = sb->st_dev;
    t->items[t->len].ino = sb->st_ino;
    t->len++;
}

/* src_info: hashed without the name but COMPARED with it
   (triple_hash_no_name + triple_compare -> same inode AND same_name):
   `cp a ./a d` matches, hardlink pairs `cp f g d` do not. */
bool
chopin_src_seen_or_record(const char *name, const struct stat *sb)
{
    for (size_t i = 0; i < src_info.len; i++)
        if (src_info.items[i].dev == sb->st_dev
            && src_info.items[i].ino == sb->st_ino
            && chopin_same_nameat(AT_FDCWD, src_info.items[i].name,
                                  AT_FDCWD, name) == 1)
            return true;
    table_add(&src_info, name, sb);
    return false;
}

/* dest_info: name AND inode must match (triple_compare). */
bool
chopin_dest_seen(const char *relname, const struct stat *sb)
{
    for (size_t i = 0; i < dest_info.len; i++)
        if (dest_info.items[i].dev == sb->st_dev
            && dest_info.items[i].ino == sb->st_ino
            && strcmp(dest_info.items[i].name, relname) == 0)
            return true;
    return false;
}

void
chopin_dest_record(const char *relname, const struct stat *sb)
{
    table_add(&dest_info, relname, sb);
}

/* --- src_to_dest ----------------------------------------------------
   Open addressing keyed on (dev, ino). This table sees TREE-scale
   traffic - one lookup per multi-link entry - and the original
   linear scan was O(groups x links): 3.07% of cycles on the
   hardlink-farm profile (6x GNU's entire gnulib-hash cost) and the
   whole 1.2x lane loss. src_info/dest_info keep their linear scans:
   they hold command-line operands only. Deletions (forget_created:
   failure paths, un_backup) use tombstones. */

struct s2d_slot {
    char *name;     /* NULL = empty; S2D_TOMB = deleted */
    dev_t dev;
    ino_t ino;
};

static char s2d_tomb_marker;
#define S2D_TOMB (&s2d_tomb_marker)

static struct s2d_slot *s2d_tab;
static size_t s2d_cap;      /* power of two */
static size_t s2d_len;      /* live entries */

static size_t
s2d_hash(dev_t dev, ino_t ino)
{
    uint64_t h = (uint64_t)ino ^ ((uint64_t)dev * 0x9E3779B97F4A7C15ull);

    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27;
    return (size_t)h;
}

static void
s2d_grow(void)
{
    size_t ncap = s2d_cap ? s2d_cap * 2 : 64;
    struct s2d_slot *nt = chopin_xmalloc(ncap * sizeof *nt);

    memset(nt, 0, ncap * sizeof *nt);
    for (size_t i = 0; i < s2d_cap; i++) {
        if (s2d_tab[i].name == NULL || s2d_tab[i].name == S2D_TOMB)
            continue;

        size_t j = s2d_hash(s2d_tab[i].dev, s2d_tab[i].ino) & (ncap - 1);

        while (nt[j].name != NULL)
            j = (j + 1) & (ncap - 1);
        nt[j] = s2d_tab[i];
    }
    free(s2d_tab);
    s2d_tab = nt;
    s2d_cap = ncap;
}

const char *
chopin_src_to_dest_lookup(dev_t dev, ino_t ino)
{
    if (s2d_len == 0)
        return NULL;

    size_t i = s2d_hash(dev, ino) & (s2d_cap - 1);

    while (s2d_tab[i].name != NULL) {
        if (s2d_tab[i].name != S2D_TOMB
            && s2d_tab[i].dev == dev && s2d_tab[i].ino == ino)
            return s2d_tab[i].name;
        i = (i + 1) & (s2d_cap - 1);
    }
    return NULL;
}

const char *
chopin_remember_copied(const char *dest, dev_t dev, ino_t ino)
{
    const char *earlier = chopin_src_to_dest_lookup(dev, ino);

    if (earlier != NULL)
        return earlier;
    if ((s2d_len + 1) * 2 > s2d_cap)
        s2d_grow();

    size_t i = s2d_hash(dev, ino) & (s2d_cap - 1);

    while (s2d_tab[i].name != NULL && s2d_tab[i].name != S2D_TOMB)
        i = (i + 1) & (s2d_cap - 1);
    s2d_tab[i].name = chopin_xstrdup(dest);
    s2d_tab[i].dev = dev;
    s2d_tab[i].ino = ino;
    s2d_len++;
    return NULL;
}

void
chopin_forget_created(dev_t dev, ino_t ino)
{
    if (s2d_len == 0)
        return;

    size_t i = s2d_hash(dev, ino) & (s2d_cap - 1);

    while (s2d_tab[i].name != NULL) {
        if (s2d_tab[i].name != S2D_TOMB
            && s2d_tab[i].dev == dev && s2d_tab[i].ino == ino) {
            free(s2d_tab[i].name);
            s2d_tab[i].name = S2D_TOMB;
            s2d_len--;
            return;
        }
        i = (i + 1) & (s2d_cap - 1);
    }
}
