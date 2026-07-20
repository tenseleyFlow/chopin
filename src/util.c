#include "util.h"

#include <errno.h>
#include <langinfo.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

const char *chopin_prog = "chopin";
const char *chopin_argv0 = "chopin";

void
chopin_set_program(const char *argv0)
{
    const char *slash = strrchr(argv0, '/');
    chopin_argv0 = argv0;
    chopin_prog = slash ? slash + 1 : argv0;
}

static const char *quote_left = "'";
static const char *quote_right = "'";

void
chopin_diag_init(void)
{
    const char *cs = nl_langinfo(CODESET);
    if (cs && strcmp(cs, "UTF-8") == 0) {
        quote_left = "\342\200\230";    /* U+2018 */
        quote_right = "\342\200\231";   /* U+2019 */
    }
}

const char *
chopin_qL(void)
{
    return quote_left;
}

const char *
chopin_qR(void)
{
    return quote_right;
}

const char *
chopin_quote_diag(const char *name)
{
    static char *slots[2];
    static size_t caps[2];
    static int turn;

    turn = 1 - turn;
    size_t len = strlen(name);
    size_t want = len * 4 + 1;
    if (caps[turn] < want) {
        slots[turn] = chopin_xrealloc(slots[turn], want);
        caps[turn] = want;
    }
    char *out = slots[turn];
    size_t o = 0;
    size_t i = 0;
    mbstate_t st;
    memset(&st, 0, sizeof st);

    while (i < len) {
        wchar_t wc;
        size_t r = mbrtowc(&wc, name + i, len - i, &st);
        if (r != (size_t)-1 && r != (size_t)-2 && r != 0
            && iswprint((wint_t)wc) && wc != L'\\') {
            memcpy(out + o, name + i, r);
            o += r;
            i += r;
            continue;
        }
        if (r == (size_t)-1 || r == (size_t)-2)
            memset(&st, 0, sizeof st);
        unsigned char c = (unsigned char)name[i];
        i++;
        switch (c) {
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\a': out[o++] = '\\'; out[o++] = 'a'; break;
        case '\b': out[o++] = '\\'; out[o++] = 'b'; break;
        case '\t': out[o++] = '\\'; out[o++] = 't'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
        case '\v': out[o++] = '\\'; out[o++] = 'v'; break;
        case '\f': out[o++] = '\\'; out[o++] = 'f'; break;
        case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
        default:
            out[o++] = '\\';
            out[o++] = (char)('0' + ((c >> 6) & 7));
            out[o++] = (char)('0' + ((c >> 3) & 7));
            out[o++] = (char)('0' + (c & 7));
            break;
        }
    }
    out[o] = '\0';
    return out;
}

void
chopin_try_help_print(void)
{
    fprintf(stderr, "Try '%s --help' for more information.\n", chopin_argv0);
}

_Noreturn void
chopin_try_help_and_die(void)
{
    chopin_try_help_print();
    exit(CHOPIN_STATUS_FAIL);
}

static void
verror(int errnum, const char *fmt, va_list ap)
{
    fprintf(stderr, "%s: ", chopin_prog);
    vfprintf(stderr, fmt, ap);
    if (errnum != 0)
        fprintf(stderr, ": %s", strerror(errnum));
    fputc('\n', stderr);
}

void
chopin_error(int errnum, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verror(errnum, fmt, ap);
    va_end(ap);
}

_Noreturn void
chopin_die(int errnum, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verror(errnum, fmt, ap);
    va_end(ap);
    exit(CHOPIN_STATUS_FAIL);
}

void *
chopin_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        chopin_die(errno, "memory exhausted");
    return p;
}

void *
chopin_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        chopin_die(errno, "memory exhausted");
    return q;
}

char *
chopin_xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = chopin_xmalloc(n);
    memcpy(p, s, n);
    return p;
}
