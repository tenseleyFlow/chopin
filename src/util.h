#ifndef CHOPIN_UTIL_H
#define CHOPIN_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* cp has only two exit statuses (gnu-cp-analysis.md 4.1). */
enum { CHOPIN_STATUS_OK = 0, CHOPIN_STATUS_FAIL = 1 };

#if defined(__GNUC__)
#define CHOPIN_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define CHOPIN_PRINTF(f, a)
#endif

extern const char *chopin_prog;     /* basename for diagnostics */
extern const char *chopin_argv0;    /* as invoked, for Try-help */

void chopin_set_program(const char *argv0);

/* Locale-derived diagnostic quote marks (UTF-8 gets U+2018/U+2019,
   else apostrophes) - call after setlocale. */
void chopin_diag_init(void);
const char *chopin_qL(void);
const char *chopin_qR(void);

/* Escape a name for diagnostic embedding between qL/qR: locale-aware,
   C-style escapes for unprintables (gnulib locale-quoting behavior for
   the argmatch surface; full quotearg lives in quote.h). */
const char *chopin_quote_diag(const char *name);

void chopin_try_help_print(void);
_Noreturn void chopin_try_help_and_die(void);

/* GNU error() semantics: "PROG: fmt[: strerror(errnum)]\n" on stderr.
   chopin_die always exits CHOPIN_STATUS_FAIL. */
void chopin_error(int errnum, const char *fmt, ...) CHOPIN_PRINTF(2, 3);
_Noreturn void chopin_die(int errnum, const char *fmt, ...) CHOPIN_PRINTF(2, 3);

/* Diagnostic capture (sprint 09): while a capture is installed on the
   CALLING thread, chopin_error appends formatted lines to it instead
   of stderr. Workers capture into their result slot; the spine
   captures into ordered sync slots while a batch is in flight, so
   stderr replays in traversal order at barriers. chopin_die flushes
   the thread's capture to stderr before exiting - fatal oracle aborts
   stay loud and ordered. */
struct chopin_errcap {
    char *buf;
    size_t len;
    size_t cap;
};

void chopin_error_capture(struct chopin_errcap *cap);   /* NULL = direct */
struct chopin_errcap *chopin_error_capture_current(void);
void chopin_errcap_flush(struct chopin_errcap *cap);    /* to stderr */

void *chopin_xmalloc(size_t n);
void *chopin_xrealloc(void *p, size_t n);
char *chopin_xstrdup(const char *s);

/* Cached euid (never changes; GNU calls geteuid once per run, the
   uncached version showed up 2000x on the hardlink lane). */
uid_t chopin_euid(void);

#endif
