#include "copydata.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "quote.h"
#include "util.h"

/* Sprint 02: the scalar read/write engine - the verification oracle
   every fast path (sprint 07) is checked against. Buffer sizing
   follows GNU io_blksize semantics (gnu-cp-analysis.md 2.5): the
   SIZE is parity-shaped; the buffer itself is persistent per run
   (GNU's per-file alloc/free is pure waste and reuse is not
   observable). CHOPIN_FORCE_SCALAR routes here by definition. */

/* ioblksize.h:79-111: max(st_blksize, 256KiB) kept a multiple of
   st_blksize; non-power-of-two blksize on regular files promoted to
   the next power of two (ZFS workaround); capped. */
#define IO_BUFSIZE (256 * 1024)
#define SYS_BUFSIZE_MAX ((INT32_MAX >> 20) << 20)

static size_t
io_blksize(const struct stat *st)
{
    intmax_t blksize = st->st_blksize > 0 ? (intmax_t)st->st_blksize : 512;
    intmax_t size;

    if (blksize > SYS_BUFSIZE_MAX)
        blksize = SYS_BUFSIZE_MAX;
    size = IO_BUFSIZE / (int)blksize * (int)blksize;
    if (size < blksize)
        size = blksize;
    if (S_ISREG(st->st_mode) && (blksize & (blksize - 1)) != 0) {
        intmax_t pow2 = 1;
        while (pow2 < blksize && pow2 <= SYS_BUFSIZE_MAX / 2)
            pow2 *= 2;
        size = pow2;
    }
    if (size > SYS_BUFSIZE_MAX)
        size = SYS_BUFSIZE_MAX;
    return (size_t)size;
}

/* gnulib buffer_lcm reduced: lcm of the two block sizes, capped. */
static size_t
buffer_lcm(size_t a, size_t b)
{
    size_t x = a, y = b, t;

    if (a == 0 || b == 0)
        return a | b ? (a | b) : 1;
    while (y != 0) {
        t = x % y;
        x = y;
        y = t;
    }
    /* a / gcd * b, watching overflow */
    x = a / x;
    if (b > SIZE_MAX / x)
        return a > b ? a : b;
    return x * b;
}

/* Persistent page-aligned buffer, grown on demand. */
static char *buf;
static size_t buf_cap;

static char *
get_buffer(size_t want)
{
    if (want > buf_cap) {
        long ps = sysconf(_SC_PAGESIZE);
        void *p = NULL;

        free(buf);
        if (posix_memalign(&p, ps > 0 ? (size_t)ps : 4096, want) != 0)
            chopin_die(errno, "memory exhausted");
        buf = p;
        buf_cap = want;
    }
    return buf;
}

bool
chopin_copy_file_data(int src_fd, const struct stat *src_sb,
                      const char *src_name, int dest_fd,
                      const struct stat *dst_sb, const char *dst_name)
{
    size_t out_size = io_blksize(dst_sb);
    size_t in_size = io_blksize(src_sb);
    size_t blcm = buffer_lcm(in_size, out_size);
    size_t want = out_size;

    /* Clamp toward st_size + 1 for small regular files, then round up
       to a positive multiple of the lcm (copy-file-data.c:577-599). */
    if (S_ISREG(src_sb->st_mode) && src_sb->st_size >= 0
        && (uintmax_t)src_sb->st_size + 1 < want)
        want = (size_t)src_sb->st_size + 1;
    want += blcm - 1;
    want -= want % blcm;
    if (want == 0)
        want = blcm;

    char *b = get_buffer(want);

    /* Never early-exit on a short read (quirk 12): loop until read
       returns 0 - /proc files and pipes underfill reads. */
    for (;;) {
        ssize_t n_read = read(src_fd, b, want);

        if (n_read < 0) {
            if (errno == EINTR)
                continue;
            chopin_error(errno, "error reading %s", chopin_quoteaf(src_name));
            return false;
        }
        if (n_read == 0)
            break;

        ssize_t done = 0;
        while (done < n_read) {
            ssize_t n = write(dest_fd, b + done, (size_t)(n_read - done));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                chopin_error(errno, "error writing %s",
                             chopin_quoteaf(dst_name));
                return false;
            }
            done += n;
        }
    }
    return true;
}
