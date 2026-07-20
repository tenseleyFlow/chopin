#ifndef CHOPIN_UTIL_H
#define CHOPIN_UTIL_H

#include <stdbool.h>
#include <stddef.h>

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

void *chopin_xmalloc(size_t n);
void *chopin_xrealloc(void *p, size_t n);
char *chopin_xstrdup(const char *s);

#endif
