#include "hashes.h"

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

/* --- src_to_dest ---------------------------------------------------- */

static struct table src_to_dest;

const char *
chopin_src_to_dest_lookup(dev_t dev, ino_t ino)
{
    for (size_t i = 0; i < src_to_dest.len; i++)
        if (src_to_dest.items[i].dev == dev
            && src_to_dest.items[i].ino == ino)
            return src_to_dest.items[i].name;
    return NULL;
}

const char *
chopin_remember_copied(const char *dest, dev_t dev, ino_t ino)
{
    const char *earlier = chopin_src_to_dest_lookup(dev, ino);

    if (earlier != NULL)
        return earlier;
    struct stat key;
    key.st_dev = dev;
    key.st_ino = ino;
    table_add(&src_to_dest, dest, &key);
    return NULL;
}

void
chopin_forget_created(dev_t dev, ino_t ino)
{
    for (size_t i = 0; i < src_to_dest.len; i++)
        if (src_to_dest.items[i].dev == dev
            && src_to_dest.items[i].ino == ino) {
            free(src_to_dest.items[i].name);
            src_to_dest.items[i] = src_to_dest.items[--src_to_dest.len];
            return;
        }
}
