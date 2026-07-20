#ifndef CHOPIN_QUOTE_H
#define CHOPIN_QUOTE_H

#include <stdbool.h>
#include <stddef.h>

/* Port of gnulib quotearg's buffer state machine (via liszt's verified
   copy), reduced to what cp's diagnostics use:
     quoteaf(x) = quotearg_style(shell_escape_always_quoting_style, x)
     quotef(x)  = quotearg_n_style_colon(0, shell_escape_quoting_style, x)
   (coreutils src/system.h definitions). */

enum chopin_qstyle {
    CHOPIN_QS_LITERAL = 0,
    CHOPIN_QS_SHELL,
    CHOPIN_QS_SHELL_ALWAYS,
    CHOPIN_QS_SHELL_ESCAPE,
    CHOPIN_QS_SHELL_ESCAPE_ALWAYS,
    CHOPIN_QS_C,
    CHOPIN_QS_C_MAYBE,
    CHOPIN_QS_ESCAPE,
    CHOPIN_QS_LOCALE,
    CHOPIN_QS_CLOCALE
};

struct chopin_qopts {
    enum chopin_qstyle style;
    unsigned int quote_these_too[8];    /* 256-bit set */
};

void chopin_set_char_quoting(struct chopin_qopts *o, char c, int on);

/* quotearg_buffer semantics: writes at most BUFSIZE bytes (always
   NUL-terminating when it fits), returns the full quoted length. */
size_t chopin_quotearg_buffer(char *buf, size_t bufsize, const char *arg,
                              size_t argsize, const struct chopin_qopts *o);

/* Rotating-slot renderers (gnulib quotearg_n: results in independent
   static slots so two quoted names can appear in one format call). */
const char *chopin_quoteaf(const char *arg);
const char *chopin_quoteaf_n(int n, const char *arg);
const char *chopin_quotef(const char *arg);

/* Sprint 09: the slots are per-thread so pool workers can format
   diagnostics; free a worker's copies at pool teardown. */
void chopin_quote_thread_cleanup(void);

#endif
