#include "quote.h"

#include <ctype.h>
#include <langinfo.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "util.h"

/* Faithful port of quotearg_buffer_restyled (lib/quotearg.c, pinned
   tree) via liszt's verified copy: same control flow, same gotos, same
   store discipline. The ls-only width machinery is dropped; cp never
   measures display width. wchar_t is UTF-32 on every supported
   platform, so mbrtowc/iswprint stand in for mbrtoc32/c32isprint. */

enum { INT_BITS = (int)(sizeof (int) * CHAR_BIT) };

void
chopin_set_char_quoting(struct chopin_qopts *o, char c, int on)
{
    unsigned char uc = (unsigned char)c;
    unsigned int *w = &o->quote_these_too[uc / INT_BITS];
    unsigned int bit = 1u << (uc % INT_BITS);

    if (on)
        *w |= bit;
    else
        *w &= ~bit;
}

/* Per-thread (sprint 09): pool workers format diagnostics with the
   same quoting entry points; thread-local state keeps them race-free
   without a lock in the hot path. chopin_quote_thread_cleanup frees a
   worker's copies at pool teardown. */
static _Thread_local signed char *iswprint_tab; /* iswprint + 1 */

static int
cached_iswprint(wchar_t wc)
{
    if ((unsigned long)wc >= 0x10000ul)
        return iswprint((wint_t)wc);
    if (!iswprint_tab) {
        iswprint_tab = chopin_xmalloc(0x10000);
        memset(iswprint_tab, 0, 0x10000);
    }
    if (iswprint_tab[wc] == 0)
        iswprint_tab[wc] = (signed char)(iswprint((wint_t)wc) ? 2 : 1);
    return iswprint_tab[wc] - 1;
}

/* gettext_quote reduced: no message catalogs; UTF-8 locales get curly
   quotes, else clocale quotes with '"' and locale with "'". */
static const char *
gettext_quote(bool left, enum chopin_qstyle s)
{
    const char *cs = nl_langinfo(CODESET);

    if (cs && strcmp(cs, "UTF-8") == 0)
        return left ? "\342\200\230" : "\342\200\231";
    return s == CHOPIN_QS_CLOCALE ? "\"" : "'";
}

static size_t
quotearg_restyled(char *buffer, size_t buffersize, const char *arg,
                  size_t argsize, enum chopin_qstyle quoting_style,
                  const unsigned int *quote_these_too)
{
    bool unibyte_locale = MB_CUR_MAX == 1;
    size_t len = 0;
    size_t orig_buffersize = 0;
    const char *quote_string = NULL;
    size_t quote_string_len = 0;
    bool backslash_escapes = false;
    bool elide_outer_quotes = false;
    bool encountered_single_quote = false;
    bool all_c_and_shell_quote_compat = true;
    const char *left_quote = NULL;
    const char *right_quote = NULL;
    bool pending_shell_escape_end;

#define STORE(ch) \
    do { \
        if (len < buffersize) \
            buffer[len] = (ch); \
        len++; \
    } while (0)

#define START_ESC() \
    do { \
        if (elide_outer_quotes) \
            goto force_outer_quoting_style; \
        escaping = true; \
        if (quoting_style == CHOPIN_QS_SHELL_ALWAYS \
            && !pending_shell_escape_end) { \
            STORE('\''); \
            STORE('$'); \
            STORE('\''); \
            pending_shell_escape_end = true; \
        } \
        STORE('\\'); \
    } while (0)

#define END_ESC() \
    do { \
        if (pending_shell_escape_end && !escaping) { \
            STORE('\''); \
            STORE('\''); \
            pending_shell_escape_end = false; \
        } \
    } while (0)

process_input:
    pending_shell_escape_end = false;

    switch (quoting_style) {
    case CHOPIN_QS_C_MAYBE:
        quoting_style = CHOPIN_QS_C;
        elide_outer_quotes = true;
        /* fall through */
    case CHOPIN_QS_C:
        if (!elide_outer_quotes)
            STORE('"');
        backslash_escapes = true;
        quote_string = "\"";
        quote_string_len = 1;
        break;

    case CHOPIN_QS_ESCAPE:
        backslash_escapes = true;
        elide_outer_quotes = false;
        break;

    case CHOPIN_QS_LOCALE:
    case CHOPIN_QS_CLOCALE:
        left_quote = gettext_quote(true, quoting_style);
        right_quote = gettext_quote(false, quoting_style);
        if (!elide_outer_quotes)
            for (const char *lq = left_quote; *lq; lq++)
                STORE(*lq);
        backslash_escapes = true;
        quote_string = right_quote;
        quote_string_len = strlen(quote_string);
        break;

    case CHOPIN_QS_SHELL_ESCAPE:
        backslash_escapes = true;
        /* fall through */
    case CHOPIN_QS_SHELL:
        elide_outer_quotes = true;
        /* fall through */
    case CHOPIN_QS_SHELL_ESCAPE_ALWAYS:
        if (!elide_outer_quotes)
            backslash_escapes = true;
        /* fall through */
    case CHOPIN_QS_SHELL_ALWAYS:
        quoting_style = CHOPIN_QS_SHELL_ALWAYS;
        if (!elide_outer_quotes)
            STORE('\'');
        quote_string = "'";
        quote_string_len = 1;
        break;

    case CHOPIN_QS_LITERAL:
    default:
        elide_outer_quotes = false;
        break;
    }

    for (size_t i = 0;
         !(argsize == (size_t)-1 ? arg[i] == '\0' : i == argsize); i++) {
        bool is_right_quote = false;
        bool escaping = false;
        bool c_and_shell_quote_compat = false;
        unsigned char c;
        unsigned char esc;

        if (backslash_escapes && quoting_style != CHOPIN_QS_SHELL_ALWAYS
            && quote_string_len
            && (i + quote_string_len
                <= (argsize == (size_t)-1 && 1 < quote_string_len
                    ? (argsize = strlen(arg)) : argsize))
            && memcmp(arg + i, quote_string, quote_string_len) == 0) {
            if (elide_outer_quotes)
                goto force_outer_quoting_style;
            is_right_quote = true;
        }

        c = (unsigned char)arg[i];
        switch (c) {
        case '\0':
            if (backslash_escapes) {
                START_ESC();
                if (quoting_style != CHOPIN_QS_SHELL_ALWAYS
                    && i + 1 < argsize && '0' <= arg[i + 1]
                    && arg[i + 1] <= '9') {
                    STORE('0');
                    STORE('0');
                }
                c = '0';
            }
            break;

        case '?':
            if (quoting_style == CHOPIN_QS_SHELL_ALWAYS
                && elide_outer_quotes)
                goto force_outer_quoting_style;
            break;

        case '\a': esc = 'a'; goto c_escape;
        case '\b': esc = 'b'; goto c_escape;
        case '\f': esc = 'f'; goto c_escape;
        case '\n': esc = 'n'; goto c_and_shell_escape;
        case '\r': esc = 'r'; goto c_and_shell_escape;
        case '\t': esc = 't'; goto c_and_shell_escape;
        case '\v': esc = 'v'; goto c_escape;
        case '\\':
            esc = c;
            if (quoting_style == CHOPIN_QS_SHELL_ALWAYS) {
                if (elide_outer_quotes)
                    goto force_outer_quoting_style;
                goto store_c;
            }
            if (backslash_escapes && elide_outer_quotes
                && quote_string_len)
                goto store_c;
        c_and_shell_escape:
            if (quoting_style == CHOPIN_QS_SHELL_ALWAYS
                && elide_outer_quotes)
                goto force_outer_quoting_style;
            /* fall through */
        c_escape:
            if (backslash_escapes) {
                c = esc;
                goto store_escape;
            }
            break;

        case '{': case '}':     /* sometimes special if isolated */
            if (!(argsize == (size_t)-1 ? arg[1] == '\0' : argsize == 1))
                break;
            /* fall through */
        case '#': case '~':
            if (i != 0)
                break;
            /* fall through */
        case ' ':
            c_and_shell_quote_compat = true;
            /* fall through */
        case '!':
        case '"': case '$': case '&':
        case '(': case ')': case '*': case ';':
        case '<':
        case '=':
        case '>': case '[':
        case '^':
        case '`': case '|':
            if (quoting_style == CHOPIN_QS_SHELL_ALWAYS
                && elide_outer_quotes)
                goto force_outer_quoting_style;
            break;

        case '\'':
            encountered_single_quote = true;
            c_and_shell_quote_compat = true;
            if (quoting_style == CHOPIN_QS_SHELL_ALWAYS) {
                if (elide_outer_quotes)
                    goto force_outer_quoting_style;
                if (buffersize && !orig_buffersize) {
                    orig_buffersize = buffersize;
                    buffersize = 0;
                }
                STORE('\'');
                STORE('\\');
                STORE('\'');
                pending_shell_escape_end = false;
            }
            break;

        case '%': case '+': case ',': case '-': case '.': case '/':
        case '0': case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9': case ':':
        case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
        case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
        case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
        case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
        case 'Y': case 'Z': case ']': case '_': case 'a': case 'b':
        case 'c': case 'd': case 'e': case 'f': case 'g': case 'h':
        case 'i': case 'j': case 'k': case 'l': case 'm': case 'n':
        case 'o': case 'p': case 'q': case 'r': case 's': case 't':
        case 'u': case 'v': case 'w': case 'x': case 'y': case 'z':
            c_and_shell_quote_compat = true;
            break;

        default: {
            size_t m;
            bool printable;

            if (unibyte_locale) {
                m = 1;
                printable = isprint(c) != 0;
            } else {
                mbstate_t mbs;
                memset(&mbs, 0, sizeof mbs);
                m = 0;
                printable = true;
                if (argsize == (size_t)-1)
                    argsize = strlen(arg);
                for (;;) {
                    wchar_t w;
                    size_t bytes = mbrtowc(&w, &arg[i + m],
                                           argsize - (i + m), &mbs);
                    if (bytes == 0) {
                        break;
                    } else if (bytes == (size_t)-1) {
                        printable = false;
                        break;
                    } else if (bytes == (size_t)-2) {
                        printable = false;
                        while (i + m < argsize && arg[i + m])
                            m++;
                        break;
                    } else {
                        if (elide_outer_quotes
                            && quoting_style == CHOPIN_QS_SHELL_ALWAYS) {
                            for (size_t j = 1; j < bytes; j++)
                                switch (arg[i + m + j]) {
                                case '[': case '\\': case '^':
                                case '`': case '|':
                                    goto force_outer_quoting_style;
                                }
                        }
                        if (!cached_iswprint(w))
                            printable = false;
                        m += bytes;
                    }
                    break;
                }
            }
            c_and_shell_quote_compat = printable;

            if (1 < m || (backslash_escapes && !printable)) {
                size_t ilim = i + m;

                for (;;) {
                    if (backslash_escapes && !printable) {
                        START_ESC();
                        STORE((char)('0' + (c >> 6)));
                        STORE((char)('0' + ((c >> 3) & 7)));
                        c = (unsigned char)('0' + (c & 7));
                    } else if (is_right_quote) {
                        STORE('\\');
                        is_right_quote = false;
                    }
                    if (ilim <= i + 1)
                        break;
                    END_ESC();
                    STORE((char)c);
                    c = (unsigned char)arg[++i];
                }
                goto store_c;
            }
        }
        }

        if (!(((backslash_escapes
                && quoting_style != CHOPIN_QS_SHELL_ALWAYS)
               || elide_outer_quotes)
              && quote_these_too
              && quote_these_too[c / INT_BITS] >> (c % INT_BITS) & 1)
            && !is_right_quote)
            goto store_c;

    store_escape:
        START_ESC();

    store_c:
        END_ESC();
        STORE((char)c);

        if (!c_and_shell_quote_compat)
            all_c_and_shell_quote_compat = false;
    }

    if (len == 0 && quoting_style == CHOPIN_QS_SHELL_ALWAYS
        && elide_outer_quotes)
        goto force_outer_quoting_style;

    if (quoting_style == CHOPIN_QS_SHELL_ALWAYS && !elide_outer_quotes
        && encountered_single_quote) {
        if (all_c_and_shell_quote_compat)
            return quotearg_restyled(buffer, orig_buffersize, arg, argsize,
                                     CHOPIN_QS_C, quote_these_too);
        else if (!buffersize && orig_buffersize) {
            buffersize = orig_buffersize;
            len = 0;
            goto process_input;
        }
    }

    if (quote_string && !elide_outer_quotes)
        for (; *quote_string; quote_string++)
            STORE(*quote_string);

    if (len < buffersize)
        buffer[len] = '\0';
    return len;

force_outer_quoting_style:
    if (quoting_style == CHOPIN_QS_SHELL_ALWAYS && backslash_escapes)
        quoting_style = CHOPIN_QS_SHELL_ESCAPE_ALWAYS;
    return quotearg_restyled(buffer, buffersize, arg, argsize,
                             quoting_style, NULL);

#undef STORE
#undef START_ESC
#undef END_ESC
}

size_t
chopin_quotearg_buffer(char *buf, size_t bufsize, const char *arg,
                       size_t argsize, const struct chopin_qopts *o)
{
    return quotearg_restyled(buf, bufsize, arg, argsize, o->style,
                             o->quote_these_too);
}

/* gnulib quotearg_n: independent growing slots. */
#define N_SLOTS 4

static _Thread_local char *slots[N_SLOTS];
static _Thread_local size_t slot_caps[N_SLOTS];

static const char *
quote_slot(int n, const struct chopin_qopts *o, const char *arg)
{
    size_t want = chopin_quotearg_buffer(slots[n], slot_caps[n], arg,
                                         (size_t)-1, o) + 1;
    if (want > slot_caps[n]) {
        slots[n] = chopin_xrealloc(slots[n], want);
        slot_caps[n] = want;
        chopin_quotearg_buffer(slots[n], slot_caps[n], arg, (size_t)-1, o);
    }
    return slots[n];
}

void
chopin_quote_thread_cleanup(void)
{
    for (int i = 0; i < N_SLOTS; i++) {
        free(slots[i]);
        slots[i] = NULL;
        slot_caps[i] = 0;
    }
    free(iswprint_tab);
    iswprint_tab = NULL;
}

const char *
chopin_quoteaf_n(int n, const char *arg)
{
    static const struct chopin_qopts o = {
        .style = CHOPIN_QS_SHELL_ESCAPE_ALWAYS
    };
    return quote_slot(n, &o, arg);
}

const char *
chopin_quoteaf(const char *arg)
{
    return chopin_quoteaf_n(0, arg);
}

const char *
chopin_quotef(const char *arg)
{
    static _Thread_local struct chopin_qopts o;
    static _Thread_local bool init;

    if (!init) {
        o.style = CHOPIN_QS_SHELL_ESCAPE;
        chopin_set_char_quoting(&o, ':', 1);
        init = true;
    }
    /* Slot 3 so quotef and quoteaf_n(0..2) can share a format call. */
    return quote_slot(3, &o, arg);
}
