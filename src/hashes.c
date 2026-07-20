#include "hashes.h"

#include <stdlib.h>
#include <string.h>

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

/* src_info: inode-only compare (triple_hash_no_name/
   triple_compare_ino). */
bool
chopin_src_seen_or_record(const char *name, const struct stat *sb)
{
    for (size_t i = 0; i < src_info.len; i++)
        if (src_info.items[i].dev == sb->st_dev
            && src_info.items[i].ino == sb->st_ino)
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
