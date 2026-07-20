#include "options.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "quote.h"
#include "util.h"

/* Option surface per gnu-cp-analysis.md s1.1: getopt string
   "abdfHilLnprst:uvxPRS:TZ" (cp.c:1052), long table cp.c:115-151.
   The parser is hand-rolled (liszt lineage) so GNU getopt behavior -
   argv permutation, unambiguous long abbreviation, glibc's same-key
   disambiguation (--pa: parents+path share a key), exact diagnostic
   bytes - holds on every libc. */

enum {
    OPT_ATTRIBUTES_ONLY = 256,
    OPT_COPY_CONTENTS,
    OPT_DEBUG,
    OPT_NO_PRESERVE,
    OPT_PRESERVE,
    OPT_PARENTS,
    OPT_REFLINK,
    OPT_REMOVE_DESTINATION,
    OPT_SPARSE,
    OPT_STRIP_TRAILING_SLASHES,
    OPT_KEEP_DIRECTORY_SYMLINK,
    OPT_HELP,
    OPT_VERSION
};

enum argtype { ARG_NO, ARG_REQ, ARG_OPT };

struct longopt {
    const char *name;
    enum argtype arg;
    int key;
};

/* Order matches cp.c's table; ambiguity listings iterate in table
   order and must print candidates the same way GNU does. */
static const struct longopt longopts[] = {
    { "archive",                ARG_NO,  'a' },
    { "attributes-only",        ARG_NO,  OPT_ATTRIBUTES_ONLY },
    { "backup",                 ARG_OPT, 'b' },
    { "copy-contents",          ARG_NO,  OPT_COPY_CONTENTS },
    { "debug",                  ARG_NO,  OPT_DEBUG },
    { "dereference",            ARG_NO,  'L' },
    { "force",                  ARG_NO,  'f' },
    { "interactive",            ARG_NO,  'i' },
    { "link",                   ARG_NO,  'l' },
    { "no-clobber",             ARG_NO,  'n' },
    { "no-dereference",         ARG_NO,  'P' },
    { "no-preserve",            ARG_REQ, OPT_NO_PRESERVE },
    { "no-target-directory",    ARG_NO,  'T' },
    { "one-file-system",        ARG_NO,  'x' },
    { "parents",                ARG_NO,  OPT_PARENTS },
    { "path",                   ARG_NO,  OPT_PARENTS },  /* deprecated */
    { "preserve",               ARG_OPT, OPT_PRESERVE },
    { "recursive",              ARG_NO,  'R' },
    { "remove-destination",     ARG_NO,  OPT_REMOVE_DESTINATION },
    { "sparse",                 ARG_REQ, OPT_SPARSE },
    { "reflink",                ARG_OPT, OPT_REFLINK },
    { "strip-trailing-slashes", ARG_NO,  OPT_STRIP_TRAILING_SLASHES },
    { "suffix",                 ARG_REQ, 'S' },
    { "symbolic-link",          ARG_NO,  's' },
    { "target-directory",       ARG_REQ, 't' },
    { "update",                 ARG_OPT, 'u' },
    { "verbose",                ARG_NO,  'v' },
    { "keep-directory-symlink", ARG_NO,  OPT_KEEP_DIRECTORY_SYMLINK },
    { "context",                ARG_OPT, 'Z' },
    { "help",                   ARG_NO,  OPT_HELP },
    { "version",                ARG_NO,  OPT_VERSION },
};
#define N_LONGOPTS ((int)(sizeof longopts / sizeof longopts[0]))

static const char short_accept[] = "abdfHilLnprstuvxPRSTZ";
static const char short_witharg[] = "tS";

/* --- argmatch (gnulib semantics via liszt's verified port) --------- */

static void
print_valid_words(const char *const *words, const int *vals, int n)
{
    fprintf(stderr, "Valid arguments are:\n");
    for (int i = 0; i < n; i++) {
        if (i > 0 && vals[i] == vals[i - 1])
            fprintf(stderr, ", %s%s%s", chopin_qL(), words[i], chopin_qR());
        else
            fprintf(stderr, "%s  - %s%s%s", i ? "\n" : "",
                    chopin_qL(), words[i], chopin_qR());
    }
    fputc('\n', stderr);
}

/* Exact match wins; else unambiguous abbreviation (several matches
   agreeing on the value are fine). Fatal exit 1 with the gnulib
   diagnostic + valid-words listing + Try-help. */
static int
argmatch_die(const char *context, const char *arg,
             const char *const *words, const int *vals, int n)
{
    int match = -1;
    bool ambiguous = false;

    for (int i = 0; i < n; i++) {
        if (strcmp(words[i], arg) == 0)
            return vals[i];
        if (strncmp(words[i], arg, strlen(arg)) == 0) {
            if (match < 0)
                match = i;
            else if (vals[i] != vals[match])
                ambiguous = true;
        }
    }
    if (match >= 0 && !ambiguous)
        return vals[match];

    chopin_error(0, "%s argument %s%s%s for %s%s%s",
                 ambiguous ? "ambiguous" : "invalid",
                 chopin_qL(), chopin_quote_diag(arg), chopin_qR(),
                 chopin_qL(), context, chopin_qR());
    print_valid_words(words, vals, n);
    chopin_try_help_and_die();
}

/* --- word tables (cp.c:85-113, lib/backup-find.c:43-60) ------------ */

static const char *const sparse_words[] = { "never", "auto", "always" };
static const int sparse_vals[] = {
    CHOPIN_SPARSE_NEVER, CHOPIN_SPARSE_AUTO, CHOPIN_SPARSE_ALWAYS
};

static const char *const reflink_words[] = { "auto", "always", "never" };
static const int reflink_vals[] = {
    CHOPIN_REFLINK_AUTO, CHOPIN_REFLINK_ALWAYS, CHOPIN_REFLINK_NEVER
};

static const char *const update_words[] = {
    "all", "none", "none-fail", "older"
};
static const int update_vals[] = {
    CHOPIN_UPDATE_ALL, CHOPIN_UPDATE_NONE, CHOPIN_UPDATE_NONE_FAIL,
    CHOPIN_UPDATE_OLDER
};

static const char *const backup_words[] = {
    "none", "off", "simple", "never", "existing", "nil", "numbered", "t"
};
static const int backup_vals[] = {
    CHOPIN_BACKUP_NONE, CHOPIN_BACKUP_NONE,
    CHOPIN_BACKUP_SIMPLE, CHOPIN_BACKUP_SIMPLE,
    CHOPIN_BACKUP_NUMBERED_EXISTING, CHOPIN_BACKUP_NUMBERED_EXISTING,
    CHOPIN_BACKUP_NUMBERED, CHOPIN_BACKUP_NUMBERED
};

/* xget_version (lib/backup-find.c:69-93): explicit arg keeps the
   caller's context ("backup type"); empty/missing falls back to
   $VERSION_CONTROL under the "$VERSION_CONTROL" context; unset/empty
   env means numbered_existing. */
static enum chopin_backup
xget_version(const char *context, const char *version)
{
    if (version == NULL || *version == '\0') {
        context = "$VERSION_CONTROL";
        version = getenv("VERSION_CONTROL");
        if (version == NULL || *version == '\0')
            return CHOPIN_BACKUP_NUMBERED_EXISTING;
    }
    return (enum chopin_backup)argmatch_die(context, version, backup_words,
                                            backup_vals, 8);
}

/* --- decode_preserve_arg (cp.c:936-1026) --------------------------- */

static const char *const preserve_words[] = {
    "mode", "timestamps", "ownership", "links", "context", "xattr", "all"
};
enum {
    PRESERVE_MODE, PRESERVE_TIMESTAMPS, PRESERVE_OWNERSHIP,
    PRESERVE_LINK, PRESERVE_CONTEXT, PRESERVE_XATTR, PRESERVE_ALL
};
static const int preserve_vals[] = {
    PRESERVE_MODE, PRESERVE_TIMESTAMPS, PRESERVE_OWNERSHIP,
    PRESERVE_LINK, PRESERVE_CONTEXT, PRESERVE_XATTR, PRESERVE_ALL
};

static void
decode_preserve_arg(const char *arg, struct chopin_options *x, bool on_off)
{
    char *writable = chopin_xstrdup(arg);
    char *s = writable;

    do {
        char *comma = strchr(s, ',');
        if (comma)
            *comma++ = '\0';

        int val = argmatch_die(on_off ? "--preserve" : "--no-preserve",
                               s, preserve_words, preserve_vals, 7);
        switch (val) {
        case PRESERVE_MODE:
            x->preserve_mode = on_off;
            x->explicit_no_preserve_mode = !on_off;
            break;
        case PRESERVE_TIMESTAMPS:
            x->preserve_timestamps = on_off;
            break;
        case PRESERVE_OWNERSHIP:
            x->preserve_ownership = on_off;
            break;
        case PRESERVE_LINK:
            x->preserve_links = on_off;
            break;
        case PRESERVE_CONTEXT:
            x->require_preserve_context = on_off;
            x->preserve_security_context = on_off;
            break;
        case PRESERVE_XATTR:
            x->preserve_xattr = on_off;
            x->require_preserve_xattr = on_off;
            break;
        case PRESERVE_ALL:
            x->preserve_mode = on_off;
            x->preserve_timestamps = on_off;
            x->preserve_ownership = on_off;
            x->preserve_links = on_off;
            x->explicit_no_preserve_mode = !on_off;
            /* +context iff selinux kernel: never, in chopin */
            x->preserve_xattr = on_off;
            break;
        default:
            chopin_die(0, "internal: preserve word");
        }
        s = comma;
    } while (s);

    free(writable);
}

/* --- help / version (own text, liszt style: NOT parity surface) ---- */

static _Noreturn void
print_help(void)
{
    printf("Usage: %s [OPTION]... [-T] SOURCE DEST\n", chopin_prog);
    printf("  or:  %s [OPTION]... SOURCE... DIRECTORY\n", chopin_prog);
    printf("  or:  %s [OPTION]... -t DIRECTORY SOURCE...\n", chopin_prog);
    printf("Copy SOURCE to DEST, or multiple SOURCE(s) to DIRECTORY.\n");
    printf("\nchopin is a from-scratch reimplementation of GNU cp,\n"
           "behavior-faithful to coreutils 9.11 with registered fixes\n"
           "(see doc/deviations.md). It accepts the full GNU cp 9.11\n"
           "option surface; see cp(1) for option semantics.\n");
    exit(CHOPIN_STATUS_OK);
}

static _Noreturn void
print_version(void)
{
    printf("%s %s\n", chopin_prog, CHOPIN_VERSION);
    exit(CHOPIN_STATUS_OK);
}

/* --- option effects (cp.c getopt switch, 1052-1246) ---------------- */

static void
handle(int key, const char *value, struct chopin_invocation *inv)
{
    struct chopin_options *x = &inv->x;

    switch (key) {
    case OPT_SPARSE:
        x->sparse_mode = (enum chopin_sparse)
            argmatch_die("--sparse", value, sparse_words, sparse_vals, 3);
        break;
    case OPT_REFLINK:
        if (value == NULL)
            x->reflink_mode = CHOPIN_REFLINK_ALWAYS;
        else
            x->reflink_mode = (enum chopin_reflink)
                argmatch_die("--reflink", value, reflink_words,
                             reflink_vals, 3);
        break;
    case 'a':
        x->dereference = CHOPIN_DEREF_NEVER;
        x->preserve_links = true;
        x->preserve_ownership = true;
        x->preserve_mode = true;
        x->preserve_timestamps = true;
        x->require_preserve = true;
        /* preserve_security_context iff selinux kernel: never */
        x->preserve_xattr = true;
        x->reduce_diagnostics = true;
        x->recursive = true;
        break;
    case 'b':
        inv->make_backups = true;
        if (value)
            inv->version_control_string = value;
        break;
    case OPT_ATTRIBUTES_ONLY:
        x->data_copy_required = false;
        break;
    case OPT_DEBUG:
        x->debug = x->verbose = true;
        break;
    case OPT_COPY_CONTENTS:
        inv->copy_contents = true;
        break;
    case 'd':
        x->preserve_links = true;
        x->dereference = CHOPIN_DEREF_NEVER;
        break;
    case 'f':
        x->unlink_dest_after_failed_open = true;
        break;
    case 'H':
        x->dereference = CHOPIN_DEREF_COMMAND_LINE_ARGUMENTS;
        break;
    case 'i':
        x->interactive = CHOPIN_I_ASK_USER;
        break;
    case 'l':
        x->hard_link = true;
        break;
    case 'L':
        x->dereference = CHOPIN_DEREF_ALWAYS;
        break;
    case 'n':
        x->interactive = CHOPIN_I_ALWAYS_SKIP;
        break;
    case 'P':
        x->dereference = CHOPIN_DEREF_NEVER;
        break;
    case OPT_NO_PRESERVE:
        decode_preserve_arg(value, x, false);
        break;
    case OPT_PRESERVE:
        if (value != NULL) {
            decode_preserve_arg(value, x, true);
            x->require_preserve = true;
            break;
        }
        /* fall through: bare --preserve == -p (cp.c:1141-1152) */
        /* FALLTHROUGH */
    case 'p':
        x->preserve_ownership = true;
        x->preserve_mode = true;
        x->preserve_timestamps = true;
        x->require_preserve = true;
        break;
    case OPT_PARENTS:
        inv->parents_option = true;
        break;
    case 'r':
    case 'R':
        x->recursive = true;
        break;
    case OPT_REMOVE_DESTINATION:
        x->unlink_dest_before_opening = true;
        break;
    case OPT_STRIP_TRAILING_SLASHES:
        inv->remove_trailing_slashes = true;
        break;
    case 's':
        x->symbolic_link = true;
        break;
    case 't':
        if (inv->target_directory)
            chopin_die(0, "multiple target directories specified");
        inv->target_directory = value;
        break;
    case 'T':
        inv->no_target_directory = true;
        break;
    case 'u':
        x->update = CHOPIN_UPDATE_OLDER;
        if (value)
            x->update = (enum chopin_update)
                argmatch_die("--update", value, update_words,
                             update_vals, 4);
        break;
    case 'v':
        x->verbose = true;
        break;
    case OPT_KEEP_DIRECTORY_SYMLINK:
        x->keep_directory_symlink = true;
        break;
    case 'x':
        x->one_file_system = true;
        break;
    case 'Z':
        /* Non-SELinux build parity (cp.c:1212-1232 else-branch):
           with arg warn, bare silently ignore. */
        if (value)
            chopin_error(0, "warning: ignoring --context; "
                            "it requires an SELinux-enabled kernel");
        break;
    case 'S':
        inv->make_backups = true;
        x->backup_suffix = value;
        break;
    case OPT_HELP:
        print_help();
    case OPT_VERSION:
        print_version();
    default:
        chopin_try_help_and_die();
    }
}

/* --- long/short parsing (liszt lineage, glibc-exact bytes) --------- */

static const struct longopt *
match_long(const char *text, size_t namelen, const char *whole_arg)
{
    const struct longopt *exact = NULL;
    const struct longopt *match = NULL;
    bool ambiguous = false;

    for (int i = 0; i < N_LONGOPTS; i++) {
        const struct longopt *lo = &longopts[i];
        if (strlen(lo->name) == namelen
            && strncmp(lo->name, text, namelen) == 0) {
            exact = lo;
            break;
        }
        if (strlen(lo->name) > namelen
            && strncmp(lo->name, text, namelen) == 0) {
            if (!match)
                match = lo;
            else if (lo->key != match->key)
                /* glibc accepts when all candidates share the key
                   (--pa: parents+path). */
                ambiguous = true;
        }
    }
    if (exact)
        return exact;
    if (match && !ambiguous)
        return match;

    if (ambiguous) {
        fprintf(stderr, "%s: option '--%.*s' is ambiguous; possibilities:",
                chopin_argv0, (int)namelen, text);
        for (int i = 0; i < N_LONGOPTS; i++) {
            if (strlen(longopts[i].name) < namelen
                || strncmp(longopts[i].name, text, namelen) != 0)
                continue;
            /* glibc omits same-key duplicates of an already-listed
               candidate (--p lists parents+preserve, not path). */
            bool dup = false;
            for (int j = 0; j < i; j++)
                if (strlen(longopts[j].name) >= namelen
                    && strncmp(longopts[j].name, text, namelen) == 0
                    && longopts[j].key == longopts[i].key) {
                    dup = true;
                    break;
                }
            if (!dup)
                fprintf(stderr, " '--%s'", longopts[i].name);
        }
        fputc('\n', stderr);
        chopin_try_help_and_die();
    }
    fprintf(stderr, "%s: unrecognized option '%s'\n", chopin_argv0,
            whole_arg);
    chopin_try_help_and_die();
}

static void
parse_long(const char *arg, int argc, char **argv, int *i,
           struct chopin_invocation *inv)
{
    const char *text = arg + 2;
    const char *eq = strchr(text, '=');
    size_t namelen = eq ? (size_t)(eq - text) : strlen(text);
    const struct longopt *lo = match_long(text, namelen, arg);
    const char *value = NULL;

    switch (lo->arg) {
    case ARG_NO:
        if (eq) {
            fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n",
                    chopin_argv0, lo->name);
            chopin_try_help_and_die();
        }
        break;
    case ARG_REQ:
        if (eq) {
            value = eq + 1;
        } else if (*i + 1 < argc) {
            value = argv[++*i];
        } else {
            fprintf(stderr, "%s: option '--%s' requires an argument\n",
                    chopin_argv0, lo->name);
            chopin_try_help_and_die();
        }
        break;
    case ARG_OPT:
        value = eq ? eq + 1 : NULL;
        break;
    }
    handle(lo->key, value, inv);
}

static void
parse_shorts(const char *arg, int argc, char **argv, int *i,
             struct chopin_invocation *inv)
{
    for (const char *p = arg + 1; *p; p++) {
        char c = *p;

        if (!strchr(short_accept, c)) {
            fprintf(stderr, "%s: invalid option -- '%c'\n", chopin_argv0, c);
            chopin_try_help_and_die();
        }
        if (strchr(short_witharg, c)) {
            const char *value;
            if (p[1] != '\0') {
                value = p + 1;
            } else if (*i + 1 < argc) {
                value = argv[++*i];
            } else {
                fprintf(stderr, "%s: option requires an argument -- '%c'\n",
                        chopin_argv0, c);
                chopin_try_help_and_die();
            }
            handle(c, value, inv);
            return;     /* the rest of the cluster was the argument */
        }
        handle(c, NULL, inv);
    }
}

void
chopin_parse_args(int argc, char **argv, struct chopin_invocation *inv)
{
    /* cp_option_init defaults (cp.c:882-932). */
    memset(inv, 0, sizeof *inv);
    inv->x.copy_as_regular = true;
    inv->x.dereference = CHOPIN_DEREF_UNDEFINED;
    inv->x.interactive = CHOPIN_I_UNSPECIFIED;
    inv->x.reflink_mode = CHOPIN_REFLINK_AUTO;
    inv->x.sparse_mode = CHOPIN_SPARSE_AUTO;
    inv->x.update = CHOPIN_UPDATE_ALL;
    inv->x.data_copy_required = true;
    inv->x.backup_suffix = NULL;
    inv->x.open_dangling_dest_symlink =
        getenv("POSIXLY_CORRECT") != NULL;

    /* GNU getopt permutes operands past options unless POSIXLY_CORRECT;
       "--" ends option scanning; "-" alone is an operand. */
    bool posixly = getenv("POSIXLY_CORRECT") != NULL;
    bool no_more_options = false;

    inv->files = chopin_xmalloc((size_t)(argc > 0 ? argc : 1)
                                * sizeof *inv->files);
    inv->n_files = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (no_more_options || arg[0] != '-' || arg[1] == '\0') {
            inv->files[inv->n_files++] = argv[i];
            if (posixly)
                no_more_options = true;
            continue;
        }
        if (strcmp(arg, "--") == 0) {
            no_more_options = true;
            continue;
        }
        if (arg[1] == '-')
            parse_long(arg, argc, argv, &i, inv);
        else
            parse_shorts(arg, argc, argv, &i, inv);
    }
}

/* --- the numbered resolution phase (cp.c:1248-1326) ---------------- */

void
chopin_options_resolve(struct chopin_invocation *inv)
{
    struct chopin_options *x = &inv->x;

    /* 1. sparse=never disables reflink (and thereby offload); FreeBSD
          copy_file_range may propagate holes (cp.c:1248-1253). */
    if (x->reflink_mode == CHOPIN_REFLINK_AUTO
        && x->sparse_mode == CHOPIN_SPARSE_NEVER)
        x->reflink_mode = CHOPIN_REFLINK_NEVER;

    /* 2. */
    if (x->hard_link && x->symbolic_link) {
        chopin_error(0, "cannot make both hard and symbolic links");
        chopin_try_help_and_die();
    }

    /* 3. -n beats any --update word regardless of order. */
    if (x->interactive == CHOPIN_I_ALWAYS_SKIP)
        x->update = CHOPIN_UPDATE_NONE;

    /* 4. */
    if (inv->make_backups
        && (x->update == CHOPIN_UPDATE_NONE
            || x->update == CHOPIN_UPDATE_NONE_FAIL)) {
        chopin_error(0, "--backup is mutually exclusive with -n or "
                        "--update=none-fail");
        chopin_try_help_and_die();
    }

    /* 5. */
    if (x->reflink_mode == CHOPIN_REFLINK_ALWAYS
        && x->sparse_mode != CHOPIN_SPARSE_AUTO) {
        chopin_error(0, "--reflink can be used only with --sparse=auto");
        chopin_try_help_and_die();
    }

    /* 6. backup_type + suffix registration (suffix env rules and
          validation land in sprint 03 with the backup engine). */
    x->backup_type = inv->make_backups
        ? xget_version("backup type", inv->version_control_string)
        : CHOPIN_BACKUP_NONE;

    /* 7. DEREF_UNDEFINED default; cp -R -l defaults to -L. */
    if (x->dereference == CHOPIN_DEREF_UNDEFINED) {
        if (x->recursive && !x->hard_link)
            x->dereference = CHOPIN_DEREF_NEVER;
        else
            x->dereference = CHOPIN_DEREF_ALWAYS;
    }

    /* 8. */
    if (x->recursive)
        x->copy_as_regular = inv->copy_contents;

    /* 9. -Z overrides -a's context preservation: no-op in chopin
          (set_security_context/scontext never set - non-SELinux). */

    /* 10. "cannot set target context and preserve it": unreachable
           (scontext always NULL); kept for numbering parity. */

    /* 11. Reachable via --preserve=context. */
    if (x->require_preserve_context)
        chopin_die(0, "cannot preserve security context "
                      "without an SELinux-enabled kernel");

    /* 12. setfscreatecon: unreachable (scontext always NULL). */

    /* 13. Message embeds the utility name as GNU's does; only
           reachable on builds without xattr syscalls (revisited for
           FreeBSD extattr in sprint 04). */
#if !CHOPIN_HAVE_GETXATTR && !CHOPIN_HAVE_GETXATTR_DARWIN
    if (x->require_preserve_xattr)
        chopin_die(0, "cannot preserve extended attributes, %s is "
                      "built without xattr support", chopin_prog);
#endif
}

/* --- debug dump ---------------------------------------------------- */

static const char *
deref_name(enum chopin_deref d)
{
    switch (d) {
    case CHOPIN_DEREF_UNDEFINED: return "undefined";
    case CHOPIN_DEREF_NEVER: return "never";
    case CHOPIN_DEREF_COMMAND_LINE_ARGUMENTS: return "cmdline";
    case CHOPIN_DEREF_ALWAYS: return "always";
    }
    return "?";
}

static const char *
interactive_name(enum chopin_interactive i)
{
    switch (i) {
    case CHOPIN_I_UNSPECIFIED: return "unspecified";
    case CHOPIN_I_ALWAYS_SKIP: return "skip";
    case CHOPIN_I_ASK_USER: return "ask";
    case CHOPIN_I_ALWAYS_YES: return "yes";
    }
    return "?";
}

static const char *
update_name(enum chopin_update u)
{
    switch (u) {
    case CHOPIN_UPDATE_ALL: return "all";
    case CHOPIN_UPDATE_NONE: return "none";
    case CHOPIN_UPDATE_NONE_FAIL: return "none-fail";
    case CHOPIN_UPDATE_OLDER: return "older";
    }
    return "?";
}

static const char *
backup_name(enum chopin_backup b)
{
    switch (b) {
    case CHOPIN_BACKUP_NONE: return "none";
    case CHOPIN_BACKUP_SIMPLE: return "simple";
    case CHOPIN_BACKUP_NUMBERED_EXISTING: return "existing";
    case CHOPIN_BACKUP_NUMBERED: return "numbered";
    }
    return "?";
}

static const char *
words3(int v, const char *const w[3])
{
    return (v >= 0 && v < 3) ? w[v] : "?";
}

void
chopin_options_dump(const struct chopin_invocation *inv, FILE *fp)
{
    static const char *const sparse_names[3] = { "never", "auto", "always" };
    static const char *const reflink_names[3] = { "never", "auto", "always" };
    const struct chopin_options *x = &inv->x;

    fprintf(fp, "dereference=%s\n", deref_name(x->dereference));
    fprintf(fp, "interactive=%s\n", interactive_name(x->interactive));
    fprintf(fp, "update=%s\n", update_name(x->update));
    fprintf(fp, "reflink=%s\n", words3((int)x->reflink_mode, reflink_names));
    fprintf(fp, "sparse=%s\n", words3((int)x->sparse_mode, sparse_names));
    fprintf(fp, "backup=%s\n", backup_name(x->backup_type));
    fprintf(fp, "backup_suffix=%s\n",
            x->backup_suffix ? x->backup_suffix : "-");
    fprintf(fp, "recursive=%d hard_link=%d symbolic_link=%d\n",
            x->recursive, x->hard_link, x->symbolic_link);
    fprintf(fp, "copy_as_regular=%d data_copy_required=%d\n",
            x->copy_as_regular, x->data_copy_required);
    fprintf(fp, "preserve mode=%d timestamps=%d ownership=%d links=%d "
                "xattr=%d\n",
            x->preserve_mode, x->preserve_timestamps, x->preserve_ownership,
            x->preserve_links, x->preserve_xattr);
    fprintf(fp, "explicit_no_preserve_mode=%d require_preserve=%d\n",
            x->explicit_no_preserve_mode, x->require_preserve);
    fprintf(fp, "force=%d remove_destination=%d verbose=%d debug=%d\n",
            x->unlink_dest_after_failed_open, x->unlink_dest_before_opening,
            x->verbose, x->debug);
    fprintf(fp, "one_file_system=%d parents=%d strip_trailing_slashes=%d "
                "keep_directory_symlink=%d\n",
            x->one_file_system, inv->parents_option,
            inv->remove_trailing_slashes, x->keep_directory_symlink);
    fprintf(fp, "attributes_only=%d open_dangling_dest_symlink=%d\n",
            !x->data_copy_required, x->open_dangling_dest_symlink);
    fprintf(fp, "target_directory=%s no_target_directory=%d\n",
            inv->target_directory ? inv->target_directory : "-",
            inv->no_target_directory);
    fprintf(fp, "n_files=%d\n", inv->n_files);
    for (int i = 0; i < inv->n_files; i++)
        fprintf(fp, "operand=%s\n", inv->files[i]);
}
