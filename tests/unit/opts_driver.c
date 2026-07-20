/* opts_driver - sprint 01 unit tables for the option surface.
 *
 * Spawns ./chopin (cwd = repo root) per case with CHOPIN_DEBUG_OPTIONS=1
 * and asserts resolved-dump lines for success cases and stderr
 * fragments + exit status for error cases. The stateful 1.3 sequence
 * orderings and the argmatch ambiguity matrix live here; the exact
 * error BYTES are the golden tier's job (vs the oracle).
 *
 * Compiled by tests/unit/run.sh, not part of the shipping binary.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ARGS 8
#define MAX_FRAGS 6

struct kase {
    const char *name;
    const char *env;            /* "VAR=VAL" or NULL */
    const char *argv[MAX_ARGS]; /* NULL-terminated, sans argv[0] */
    int want_rc;
    /* Full dump lines expected on stdout (rc 0) or substrings
       expected in stderr (rc 1). */
    const char *frags[MAX_FRAGS];
    /* Checked in stderr regardless of rc when non-NULL. */
    const char *err_frag;
};

static const struct kase kases[] = {
    /* --- stateful resolution orderings (1.3) --- */
    { "n-then-update-older", NULL, { "-n", "--update=older", "a", "b", NULL },
      0, { "interactive=skip", "update=none", NULL } },
    { "update-older-then-n", NULL, { "--update=older", "-n", "a", "b", NULL },
      0, { "update=none", NULL } },
    { "u-then-n", NULL, { "-u", "-n", "a", "b", NULL },
      0, { "update=none", NULL } },
    { "i-then-n", NULL, { "-i", "-n", "a", "b", NULL },
      0, { "interactive=skip", NULL } },
    { "n-then-i", NULL, { "-n", "-i", "a", "b", NULL },
      0, { "interactive=ask", "update=all", NULL } },
    { "f-with-n", NULL, { "-f", "-n", "a", "b", NULL },
      0, { "update=none",
           "force=1 remove_destination=0 verbose=0 debug=0", NULL } },
    /* --- deref defaulting (1.3 step 7) --- */
    { "R-deref", NULL, { "-R", "a", "b", NULL },
      0, { "dereference=never", "copy_as_regular=0 data_copy_required=1",
           NULL } },
    { "R-l-deref", NULL, { "-R", "-l", "a", "b", NULL },
      0, { "dereference=always", NULL } },
    { "l-deref", NULL, { "-l", "a", "b", NULL },
      0, { "dereference=always", NULL } },
    { "P-then-L", NULL, { "-P", "-L", "a", "b", NULL },
      0, { "dereference=always", NULL } },
    { "L-then-P", NULL, { "-L", "-P", "a", "b", NULL },
      0, { "dereference=never", NULL } },
    { "H-deref", NULL, { "-H", "a", "b", NULL },
      0, { "dereference=cmdline", NULL } },
    /* --- -a and preserve lists --- */
    { "archive", NULL, { "-a", "a", "b", NULL },
      0, { "dereference=never",
           "preserve mode=1 timestamps=1 ownership=1 links=1 xattr=1",
           "explicit_no_preserve_mode=0 require_preserve=1",
           "recursive=1 hard_link=0 symbolic_link=0", NULL } },
    { "preserve-mode", NULL, { "--preserve=mode", "a", "b", NULL },
      0, { "preserve mode=1 timestamps=0 ownership=0 links=0 xattr=0",
           "explicit_no_preserve_mode=0 require_preserve=1", NULL } },
    { "no-preserve-mode", NULL, { "--no-preserve=mode", "a", "b", NULL },
      0, { "preserve mode=0 timestamps=0 ownership=0 links=0 xattr=0",
           "explicit_no_preserve_mode=1 require_preserve=0", NULL } },
    { "no-preserve-all", NULL, { "--no-preserve=all", "a", "b", NULL },
      0, { "preserve mode=0 timestamps=0 ownership=0 links=0 xattr=0",
           "explicit_no_preserve_mode=1 require_preserve=0", NULL } },
    { "bare-preserve-is-p", NULL, { "--preserve", "a", "b", NULL },
      0, { "preserve mode=1 timestamps=1 ownership=1 links=0 xattr=0",
           NULL } },
    /* --- reflink/sparse interplay (1.3 step 1) --- */
    { "sparse-never-kills-reflink", NULL,
      { "--sparse=never", "a", "b", NULL },
      0, { "reflink=never", "sparse=never", NULL } },
    { "sparse-never-reflink-auto", NULL,
      { "--sparse=never", "--reflink=auto", "a", "b", NULL },
      0, { "reflink=never", NULL } },
    { "bare-reflink-always", NULL, { "--reflink", "a", "b", NULL },
      0, { "reflink=always", NULL } },
    /* --- backup resolution (1.3 step 6; quirk 27) --- */
    { "backup-none-then-b", NULL,
      { "--backup=none", "-b", "a", "b", NULL },
      0, { "backup=none", NULL } },
    { "b-env-t", "VERSION_CONTROL=t", { "-b", "a", "b", NULL },
      0, { "backup=numbered", NULL } },
    { "b-env-empty", "VERSION_CONTROL=", { "-b", "a", "b", NULL },
      0, { "backup=existing", NULL } },
    { "b-no-env", NULL, { "-b", "a", "b", NULL },
      0, { "backup=existing", NULL } },
    { "backup-nil", NULL, { "--backup=nil", "a", "b", NULL },
      0, { "backup=existing", NULL } },
    { "S-implies-backups", NULL, { "-S", ".bak", "a", "b", NULL },
      0, { "backup=existing", "backup_suffix=.bak", NULL } },
    /* --- long abbreviation with glibc same-key rule --- */
    { "pa-abbrev", NULL, { "--pa", "a", "/", NULL },
      0, { "one_file_system=0 parents=1 strip_trailing_slashes=0 "
           "keep_directory_symlink=0", NULL } },
    { "no-t-abbrev", NULL, { "--no-t", "a", "b", NULL },
      0, { "target_directory=- no_target_directory=1", NULL } },
    /* --- misc effects --- */
    { "copy-contents", NULL, { "-R", "--copy-contents", "a", "b", NULL },
      0, { "copy_as_regular=1 data_copy_required=1", NULL } },
    { "attributes-only", NULL, { "--attributes-only", "a", "b", NULL },
      0, { "attributes_only=1 open_dangling_dest_symlink=0", NULL } },
    { "posixly-dangling", "POSIXLY_CORRECT=1", { "a", "b", NULL },
      0, { "attributes_only=0 open_dangling_dest_symlink=1", NULL } },
    /* n_files in the dump is the parse-time operand count; the
       implicit-target decrement shows in the map lines instead. */
    { "permutation", NULL, { "a", "/", "-v", NULL },
      0, { "force=0 remove_destination=0 verbose=1 debug=0",
           "n_files=2", "map a => /a", NULL } },
    { "T-two-operands", NULL, { "-T", "a", "b", NULL },
      0, { "target_directory=- no_target_directory=1", "map a => b",
           NULL } },
    { "t-dot-shortcut", NULL, { "-t", ".", "a", NULL },
      0, { "target_directory=. no_target_directory=0", "map a => ./a",
           NULL } },
    /* --- error lane (exact bytes are golden-tier; fragments here) --- */
    { "err-hard-sym", NULL, { "-l", "-s", "a", "b", NULL },
      1, { "cannot make both hard and symbolic links", NULL } },
    { "err-backup-n", NULL, { "-b", "-n", "a", "b", NULL },
      1, { "--backup is mutually exclusive with -n or --update=none-fail",
           NULL } },
    { "err-reflink-sparse", NULL,
      { "--reflink=always", "--sparse=never", "a", "b", NULL },
      1, { "--reflink can be used only with --sparse=auto", NULL } },
    { "err-sparse-amb", NULL, { "--sparse=a", "a", "b", NULL },
      1, { "ambiguous argument 'a' for '--sparse'", NULL } },
    { "err-update-amb", NULL, { "--update=n", "a", "b", NULL },
      1, { "ambiguous argument 'n' for '--update'", NULL } },
    { "err-backup-word", NULL, { "--backup=n", "a", "b", NULL },
      1, { "ambiguous argument 'n' for 'backup type'", NULL } },
    { "err-env-backup-word", "VERSION_CONTROL=bogus",
      { "-b", "a", "b", NULL },
      1, { "invalid argument 'bogus' for '$VERSION_CONTROL'", NULL } },
    { "err-preserve-empty", NULL, { "--preserve=mode,", "a", "b", NULL },
      1, { "ambiguous argument '' for '--preserve'", NULL } },
    { "err-preserve-context", NULL,
      { "--preserve=context", "a", "b", NULL },
      1, { "cannot preserve security context without an SELinux-enabled "
           "kernel", NULL } },
    { "err-long-amb", NULL, { "--p", "a", "b", NULL },
      1, { "option '--p' is ambiguous; possibilities: '--parents' "
           "'--preserve'", NULL } },
    { "err-long-unrec", NULL, { "--bogus", "a", "b", NULL },
      1, { "unrecognized option '--bogus'", NULL } },
    { "err-short-inv", NULL, { "-q", "a", "b", NULL },
      1, { "invalid option -- 'q'", NULL } },
    { "err-long-noarg", NULL, { "--link=x", "a", "b", NULL },
      1, { "option '--link' doesn't allow an argument", NULL } },
    { "err-long-reqarg", NULL, { "--suffix", NULL },
      1, { "option '--suffix' requires an argument", NULL } },
    { "err-missing-all", NULL, { NULL },
      1, { "missing file operand", NULL } },
    { "err-missing-dst", NULL, { "onlyone", NULL },
      1, { "missing destination file operand after 'onlyone'", NULL } },
    { "err-t-T", NULL, { "-t", "x", "-T", "a", "b", NULL },
      1, { "cannot combine --target-directory (-t) and "
           "--no-target-directory (-T)", NULL } },
    { "err-extra-T", NULL, { "-T", "a", "b", "c", NULL },
      1, { "extra operand 'c'", NULL } },
    { "err-multi-t", NULL, { "-t", "x", "-t", "y", "a", NULL },
      1, { "multiple target directories specified", NULL } },
    { "err-t-missing", NULL, { "-t", "chopin-no-such-dir", "a", NULL },
      1, { "target directory 'chopin-no-such-dir': No such file or "
           "directory", NULL } },
    { "err-3op-notdir", NULL, { "a", "b", "chopin-no-such-t", NULL },
      1, { "target 'chopin-no-such-t': No such file or directory",
           NULL } },
    { "err-parents-nondir", NULL,
      { "--parents", "a", "chopin-no-such-d", NULL },
      1, { "with --parents, the destination must be a directory", NULL } },
    { "warn-Z-arg", NULL, { "--context=ctx", "a", "b", NULL },
      0, { "reflink=auto", NULL },
      .err_frag = "warning: ignoring --context; it requires an "
                  "SELinux-enabled kernel" },
};
#define N_KASES ((int)(sizeof kases / sizeof kases[0]))

static char out_path[64];
static char err_path[64];

static int
run_case(const struct kase *k, char *outbuf, char *errbuf, size_t bufsz)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        const char *argv[MAX_ARGS + 1];
        int argc = 0;
        int ofd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int efd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

        if (ofd < 0 || efd < 0)
            _exit(2);
        dup2(ofd, 1);
        dup2(efd, 2);
        /* Pin the C locale: quote marks in diagnostics are
           locale-derived (curly under UTF-8, exactly as GNU). */
        setenv("LC_ALL", "C", 1);
        setenv("CHOPIN_DEBUG_OPTIONS", "1", 1);
        if (k->env) {
            char *dup = strdup(k->env);
            char *eq = dup ? strchr(dup, '=') : NULL;
            if (!eq)
                _exit(2);
            *eq = '\0';
            setenv(dup, eq + 1, 1);
        }
        argv[argc++] = "./chopin";
        for (int i = 0; k->argv[i] && argc < MAX_ARGS; i++)
            argv[argc++] = k->argv[i];
        argv[argc] = NULL;
        execv("./chopin", (char *const *)(void *)argv);
        _exit(127);
    }

    int st;
    if (waitpid(pid, &st, 0) < 0) {
        perror("waitpid");
        exit(2);
    }

    for (int which = 0; which < 2; which++) {
        const char *path = which ? err_path : out_path;
        char *buf = which ? errbuf : outbuf;
        FILE *fp = fopen(path, "r");
        size_t n = 0;
        if (fp) {
            n = fread(buf, 1, bufsz - 1, fp);
            fclose(fp);
        }
        buf[n] = '\0';
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

/* Full-line presence for dump assertions; plain substring for stderr. */
static int
has_line(const char *hay, const char *line)
{
    size_t ll = strlen(line);
    const char *p = hay;

    while ((p = strstr(p, line)) != NULL) {
        int at_start = p == hay || p[-1] == '\n';
        int at_end = p[ll] == '\n' || p[ll] == '\0';
        if (at_start && at_end)
            return 1;
        p += 1;
    }
    return 0;
}

int
main(void)
{
    static char outbuf[65536];
    static char errbuf[65536];
    int failures = 0;

    snprintf(out_path, sizeof out_path, "build/opts_driver.out.%d",
             (int)getpid());
    snprintf(err_path, sizeof err_path, "build/opts_driver.err.%d",
             (int)getpid());

    for (int i = 0; i < N_KASES; i++) {
        const struct kase *k = &kases[i];
        int rc = run_case(k, outbuf, errbuf, sizeof outbuf);
        int bad = 0;

        if (rc != k->want_rc) {
            printf("FAIL %s: rc=%d want %d\n", k->name, rc, k->want_rc);
            bad = 1;
        }
        for (int f = 0; !bad && k->frags[f]; f++) {
            if (k->want_rc == 0) {
                if (!has_line(outbuf, k->frags[f])) {
                    printf("FAIL %s: dump lacks line [%s]\n", k->name,
                           k->frags[f]);
                    bad = 1;
                }
            } else {
                if (strstr(errbuf, k->frags[f]) == NULL) {
                    printf("FAIL %s: stderr lacks [%s]\n", k->name,
                           k->frags[f]);
                    bad = 1;
                }
            }
        }
        if (!bad && k->err_frag && strstr(errbuf, k->err_frag) == NULL) {
            printf("FAIL %s: stderr lacks [%s]\n", k->name, k->err_frag);
            bad = 1;
        }
        if (bad) {
            printf("  stdout: %.300s\n  stderr: %.300s\n", outbuf, errbuf);
            failures++;
        }
    }

    unlink(out_path);
    unlink(err_path);

    if (failures) {
        printf("opts_driver: %d/%d FAILED\n", failures, N_KASES);
        return 1;
    }
    printf("opts_driver: %d cases ok\n", N_KASES);
    return 0;
}
