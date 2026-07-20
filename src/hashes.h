#ifndef CHOPIN_HASHES_H
#define CHOPIN_HASHES_H

#include <stdbool.h>
#include <sys/stat.h>

/* The >=2-source guard tables (gnu-cp-analysis.md 2.7): F_triple
   (name, dev, ino) sets. src_info compares by inode only (so
   `cp a ./a d/` matches); dest_info compares name AND inode
   (lib/hashcode-file-inode.c triple_compare). Tables are inert until
   chopin_multi_source_init (only when 2 <= n_files, cp.c:746-753).
   src_to_dest (hardlink preservation) arrives in sprint 05. */

void chopin_multi_source_init(void);
bool chopin_multi_source_active(void);

/* Return true when (name, sb) was already recorded (per the table's
   compare rule); record it otherwise. */
bool chopin_src_seen_or_record(const char *name, const struct stat *sb);
bool chopin_dest_seen(const char *relname, const struct stat *sb);
void chopin_dest_record(const char *relname, const struct stat *sb);

/* src_to_dest (cp-hash.c): src dev/ino -> first dest name, for
   hardlink preservation and the cp -R dir dir detector. Always
   active (GNU hash_init is unconditional). remember_copied returns
   the EARLIER dest name on a hit, else records and returns NULL. */
const char *chopin_remember_copied(const char *dest, dev_t dev, ino_t ino);
const char *chopin_src_to_dest_lookup(dev_t dev, ino_t ino);
void chopin_forget_created(dev_t dev, ino_t ino);

#endif
