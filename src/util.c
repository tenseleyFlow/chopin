#include "util.h"

#include <errno.h>
#include <langinfo.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

static _Thread_local struct chopin_errcap *thread_errcap;

void
chopin_error_capture(struct chopin_errcap *cap)
{
    thread_errcap = cap;
}

struct chopin_errcap *
chopin_error_capture_current(void)
{
    return thread_errcap;
}

void
chopin_errcap_flush(struct chopin_errcap *cap)
{
    if (cap != NULL && cap->len > 0) {
        fwrite(cap->buf, 1, cap->len, stderr);
        cap->len = 0;
    }
}

static void
cap_append(struct chopin_errcap *cap, const char *s, size_t n)
{
    if (cap->len + n + 1 > cap->cap) {
        size_t want = (cap->len + n + 1) * 2;

        if (want < 256)
            want = 256;
        cap->buf = chopin_xrealloc(cap->buf, want);
        cap->cap = want;
    }
    memcpy(cap->buf + cap->len, s, n);
    cap->len += n;
    cap->buf[cap->len] = '\0';
}

static void
verror(int errnum, const char *fmt, va_list ap)
{
    struct chopin_errcap *cap = thread_errcap;

    if (cap == NULL) {
        fprintf(stderr, "%s: ", chopin_prog);
        vfprintf(stderr, fmt, ap);
        if (errnum != 0)
            fprintf(stderr, ": %s", strerror(errnum));
        fputc('\n', stderr);
        return;
    }

    /* Measure, then format - quoted path operands can be arbitrarily
       long and truncation would break parallel==serial identity. */
    char stackbody[512];
    char *body = stackbody;
    va_list aq;

    va_copy(aq, ap);
    int n = vsnprintf(stackbody, sizeof stackbody, fmt, aq);
    va_end(aq);
    if (n < 0)
        n = 0;
    if ((size_t)n >= sizeof stackbody) {
        body = chopin_xmalloc((size_t)n + 1);
        va_copy(aq, ap);
        vsnprintf(body, (size_t)n + 1, fmt, aq);
        va_end(aq);
    }

    cap_append(cap, chopin_prog, strlen(chopin_prog));
    cap_append(cap, ": ", 2);
    cap_append(cap, body, (size_t)n);
    if (body != stackbody)
        free(body);
    if (errnum != 0) {
        const char *es = strerror(errnum);

        cap_append(cap, ": ", 2);
        cap_append(cap, es, strlen(es));
    }
    cap_append(cap, "\n", 1);
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

    /* A fatal error inside a capture window must still reach the
       user: emit everything buffered so far, then the message. */
    chopin_errcap_flush(thread_errcap);
    chopin_error_capture(NULL);
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

uid_t
chopin_euid(void)
{
    static uid_t euid;
    static bool cached;

    if (!cached) {          /* primed via chopin_copy_init pre-pool */
        euid = geteuid();
        cached = true;
    }
    return euid;
}
