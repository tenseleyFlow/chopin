#ifndef CHOPIN_OPTIONS_H
#define CHOPIN_OPTIONS_H

#include <stdbool.h>
#include <stdio.h>

/* Mirrors GNU cp_options (copy.h) for the cp-relevant surface, plus
   the cp.c file-local staging the post-parse resolution consumes
   (gnu-cp-analysis.md s1). Enum shapes follow GNU where a value is
   load-bearing (REFLINK_NEVER == 0). */

enum chopin_deref {
    CHOPIN_DEREF_UNDEFINED = 1,
    CHOPIN_DEREF_NEVER,                 /* -P */
    CHOPIN_DEREF_COMMAND_LINE_ARGUMENTS,/* -H */
    CHOPIN_DEREF_ALWAYS                 /* -L */
};

enum chopin_interactive {
    CHOPIN_I_UNSPECIFIED,
    CHOPIN_I_ALWAYS_SKIP,               /* -n */
    CHOPIN_I_ASK_USER,                  /* -i */
    CHOPIN_I_ALWAYS_YES                 /* mv -f seam; never set by cp */
};

enum chopin_reflink {
    CHOPIN_REFLINK_NEVER = 0,           /* == 0 is load-bearing (quirk) */
    CHOPIN_REFLINK_AUTO,
    CHOPIN_REFLINK_ALWAYS
};

enum chopin_sparse {
    CHOPIN_SPARSE_NEVER,
    CHOPIN_SPARSE_AUTO,
    CHOPIN_SPARSE_ALWAYS
};

enum chopin_update {
    CHOPIN_UPDATE_ALL,
    CHOPIN_UPDATE_NONE,
    CHOPIN_UPDATE_NONE_FAIL,
    CHOPIN_UPDATE_OLDER
};

enum chopin_backup {
    CHOPIN_BACKUP_NONE,
    CHOPIN_BACKUP_SIMPLE,
    CHOPIN_BACKUP_NUMBERED_EXISTING,
    CHOPIN_BACKUP_NUMBERED
};

struct chopin_options {
    bool copy_as_regular;
    enum chopin_deref dereference;
    bool unlink_dest_before_opening;    /* --remove-destination */
    bool unlink_dest_after_failed_open; /* -f */
    bool hard_link;                     /* -l */
    enum chopin_interactive interactive;
    bool one_file_system;               /* -x */
    bool preserve_ownership;
    bool preserve_mode;
    bool preserve_timestamps;
    bool preserve_links;
    bool explicit_no_preserve_mode;
    bool preserve_security_context;     /* stays false: non-SELinux */
    bool require_preserve_context;
    bool preserve_xattr;
    bool require_preserve_xattr;
    bool reduce_diagnostics;            /* -a */
    bool require_preserve;
    bool recursive;
    enum chopin_backup backup_type;
    const char *backup_suffix;          /* -S; validated sprint 03 */
    bool symbolic_link;                 /* -s */
    bool data_copy_required;            /* !--attributes-only */
    enum chopin_sparse sparse_mode;
    enum chopin_reflink reflink_mode;
    enum chopin_update update;
    bool verbose;
    bool debug;
    bool keep_directory_symlink;
    bool open_dangling_dest_symlink;    /* POSIXLY_CORRECT at init */
};

/* One parsed invocation: options + operands + the cp.c-local staging
   consumed by the resolution phase. */
struct chopin_invocation {
    struct chopin_options x;
    char **files;                       /* operands after permutation */
    int n_files;
    const char *target_directory;       /* -t */
    bool no_target_directory;           /* -T */
    bool parents_option;
    bool remove_trailing_slashes;
    /* staging (pre-resolution) */
    bool make_backups;
    const char *version_control_string;
    bool copy_contents;
    const char *scontext;               /* always NULL: non-SELinux */
};

/* Parse argv (GNU permutation unless POSIXLY_CORRECT; -- terminator;
   long-option unambiguous abbreviation with glibc same-key
   disambiguation). Exits on option errors. --help/--version print and
   exit 0. */
void chopin_parse_args(int argc, char **argv, struct chopin_invocation *inv);

/* The numbered post-parse resolution phase: gnu-cp-analysis.md 1.3,
   13 steps in file order. Exits on conflict errors. */
void chopin_options_resolve(struct chopin_invocation *inv);

/* One line per field; the CHOPIN_DEBUG_OPTIONS surface the unit
   driver asserts against. */
void chopin_options_dump(const struct chopin_invocation *inv, FILE *fp);

#endif
