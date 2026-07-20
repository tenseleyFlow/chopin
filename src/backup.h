#ifndef CHOPIN_BACKUP_H
#define CHOPIN_BACKUP_H

#include <stdbool.h>

#include "options.h"

/* Backup machinery (gnu-cp-analysis.md s6; lib/backupfile.c port).
   Suffix resolution: -S argument, else $SIMPLE_BACKUP_SUFFIX when
   nonempty and slash-free, else "~". Call once at option
   resolution. */
void chopin_set_simple_backup_suffix(const char *arg);
const char *chopin_simple_backup_suffix(void);

/* find_backup_file_name: the name (malloc'd) a backup of FILE would
   use under TYPE - simple FILE~; numbered FILE.~N~ with max-scan and
   all-9s string growth; existing = numbered iff numbered present.
   Suffix replaced with ~ when NAME_MAX would overflow. NULL only on
   allocation-class failure (dir scan errors degrade to N=1 exactly
   as GNU's opendirat failure path does). */
char *chopin_find_backup_name(int dirfd, const char *file,
                              enum chopin_backup type);

/* backup_file_rename: generate the name and rename FILE to it.
   Numbered backups use RENAME_NOREPLACE and re-scan on EEXIST races.
   Returns the malloc'd backup name, NULL with errno=ENOENT tolerated
   by the caller (nothing to back up), NULL otherwise = failure. */
char *chopin_backup_file_rename(int dirfd, const char *file,
                                enum chopin_backup type);

/* un_backup: rename the backup back over FILE (failure diagnosed by
   the caller). */
bool chopin_un_backup(int dirfd, const char *backup, const char *file);

#endif
