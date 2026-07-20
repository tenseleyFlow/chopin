#include "copydata.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "quote.h"
#include "util.h"

#if CHOPIN_HAVE_FALLOCATE_PUNCH
#include <fcntl.h>
#include <linux/falloc.h>
#else
#include <fcntl.h>
#endif

/* Port of copy-file-data.c via the audit's verified walkthrough (2.6):
   scantype inference, lseek_copy extent walking, sparse_copy with the
   offload loop and word-at-a-time zero detection, tolerant hole
   punching, trailing-hole fixup. Buffer sizing is parity-shaped
   (io_blksize); the buffer itself is persistent per run. */

#define IO_BUFSIZE (256 * 1024)
#define SYS_BUFSIZE_MAX ((INT32_MAX >> 20) << 20)
#define NBLOCKSIZE 512

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

static size_t
buffer_lcm(size_t a, size_t b)
{
    size_t x = a, y = b, t;

    if (a == 0 || b == 0)
        return (a | b) ? (a | b) : 1;
    while (y != 0) {
        t = x % y;
        x = y;
        y = t;
    }
    x = a / x;
    if (b > SIZE_MAX / x)
        return a > b ? a : b;
    return x * b;
}

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

/* FICLONE probe cache (overview s5): first failure per
   (src_dev, dst_dev) pair memoizes "unsupported" so non-CoW pairs
   pay one failed ioctl per RUN, not per file. Never invalidated
   (device topology is stable within a run). */
#if CHOPIN_HAVE_FICLONE
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif
#if CHOPIN_HAVE_FCLONEFILEAT
#include <sys/attr.h>
#include <sys/clonefile.h>
#endif

struct dev_pair {
    dev_t src;
    dev_t dst;
};
static struct dev_pair *failed_pairs;
static size_t n_failed;
static size_t cap_failed;
static unsigned long clone_probes;
static unsigned long cache_hits;

static bool
pair_failed(dev_t src, dev_t dst)
{
    for (size_t i = 0; i < n_failed; i++)
        if (failed_pairs[i].src == src && failed_pairs[i].dst == dst)
            return true;
    return false;
}

static void
remember_failed_pair(dev_t src, dev_t dst)
{
    if (n_failed == cap_failed) {
        cap_failed = cap_failed ? cap_failed * 2 : 8;
        failed_pairs = chopin_xrealloc(failed_pairs,
                                       cap_failed
                                           * sizeof *failed_pairs);
    }
    failed_pairs[n_failed].src = src;
    failed_pairs[n_failed].dst = dst;
    n_failed++;
}

static void
stats_atexit(void)
{
    fprintf(stderr, "chopin stats: ficlone probes=%lu cache_hits=%lu\n",
            clone_probes, cache_hits);
}

int
chopin_clone_file(int dest_fd, int src_fd, dev_t src_dev, bool new_dst,
                  const struct chopin_options *x)
{
    static int stats = -1;

    if (stats < 0) {
        const char *e = getenv("CHOPIN_DEBUG_STATS");
        stats = e != NULL && *e != '\0' && *e != '0';
        if (stats)
            atexit(stats_atexit);
    }
    (void)new_dst;

#if CHOPIN_HAVE_FICLONE
    struct stat dsb;

    if (fstat(dest_fd, &dsb) != 0)
        return errno;
    if (x->reflink_mode != CHOPIN_REFLINK_ALWAYS
        && pair_failed(src_dev, dsb.st_dev)) {
        cache_hits++;
        return -1;
    }
    clone_probes++;
    if (ioctl(dest_fd, FICLONE, src_fd) == 0)
        return 0;
    int err = errno;
    bool terminal = err == EIO || err == ENOMEM || err == ENOSPC
        || err == EDQUOT;
    if (!terminal)
        remember_failed_pair(src_dev, dsb.st_dev);
    return err;
#elif CHOPIN_HAVE_FCLONEFILEAT
    /* macOS: fclonefileat clones by NAME into a directory; the fd
       pair shape needs the caller's dst path - the APFS engine
       completes when the mac lane carries a pinned oracle. Probe via
       the same cache discipline once implemented; report unsupported
       until then. */
    (void)dest_fd;
    (void)src_fd;
    (void)src_dev;
    (void)x;
    return ENOTSUP;
#else
    (void)dest_fd;
    (void)src_fd;
    (void)src_dev;
    (void)x;
    return ENOTSUP;
#endif
}

/* is_nul: word-at-a-time zero check (system.h:524+ shape; the SIMD
   upgrade is a sprint 10 measured decision - tally donates the
   fuzz-against-scalar pattern, not a kernel). */
static bool
is_nul(const char *b, size_t n)
{
    const unsigned long *w;
    size_t i = 0;

    while (i < n && ((uintptr_t)(b + i) % sizeof *w) != 0)
        if (b[i++] != 0)
            return false;
    w = (const unsigned long *)(const void *)(b + i);
    for (; i + sizeof *w <= n; i += sizeof *w)
        if (*w++ != 0)
            return false;
    for (; i < n; i++)
        if (b[i] != 0)
            return false;
    return true;
}

/* create_hole (copy-file-data.c:44-86): seek forward, punch the range
   behind; ENOSYS/ENOTSUP from fallocate are tolerated. Returns -1
   only on real failure - which chopin PROPAGATES (DEV-005; GNU's
   sparse_copy returns success). */
static int
create_hole(int fd, const char *name, intmax_t size)
{
    off_t file_end = lseek(fd, size, SEEK_CUR);

    if (file_end < 0) {
        chopin_error(errno, "cannot lseek %s", chopin_quoteaf(name));
        return -1;
    }
#if CHOPIN_HAVE_FALLOCATE_PUNCH
    if (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                  file_end - size, size) < 0
        && errno != ENOSYS && errno != ENOTSUP && errno != EOPNOTSUPP) {
        chopin_error(errno, "error deallocating %s", chopin_quoteaf(name));
        return -1;
    }
#endif
    return 0;
}

/* write_zeros (283-301) fills holes when the DEST cannot hold real
   holes under lseek_copy. In chopin's flow that shape routes through
   sparse_copy's plain read (holes read as zeros - same bytes, fewer
   moving parts); revisit if the sparse-to-pipe mined script diffs. */

static bool
full_write(int fd, const char *name, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            chopin_error(errno, "error writing %s", chopin_quoteaf(name));
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

enum scantype { PLAIN_SCANTYPE, ZERO_SCANTYPE, LSEEK_SCANTYPE,
                ERROR_SCANTYPE };

static enum scantype
infer_scantype(int fd, const struct stat *sb, off_t *data_start)
{
    if (!S_ISREG(sb->st_mode)
        || (intmax_t)sb->st_blocks * NBLOCKSIZE
           >= (intmax_t)sb->st_size / NBLOCKSIZE * NBLOCKSIZE)
        return PLAIN_SCANTYPE;

#if CHOPIN_HAVE_SEEK_HOLE
    off_t pos = lseek(fd, 0, SEEK_CUR);
    off_t data = lseek(fd, pos, SEEK_DATA);

    if (data >= 0) {
        /* Trivial SEEK_HOLE support (squashfs, transparent
           compression): hole at or past EOF means PLAIN. */
        off_t hole = lseek(fd, data, SEEK_HOLE);
        if (hole < 0 || hole >= sb->st_size) {
            if (lseek(fd, pos, SEEK_SET) < 0)
                return ERROR_SCANTYPE;
            return PLAIN_SCANTYPE;
        }
        if (lseek(fd, pos, SEEK_SET) < 0)
            return ERROR_SCANTYPE;
        *data_start = data;
        return LSEEK_SCANTYPE;
    }
    if (errno == ENXIO) {
        /* Entirely a hole from here. */
        if (lseek(fd, pos, SEEK_SET) < 0)
            return ERROR_SCANTYPE;
        *data_start = sb->st_size;
        return LSEEK_SCANTYPE;
    }
    if (errno == EINVAL || errno == ENOTSUP || errno == EOPNOTSUPP)
        return ZERO_SCANTYPE;
    return ERROR_SCANTYPE;
#else
    (void)fd;
    (void)data_start;
    return ZERO_SCANTYPE;
#endif
}

/* sparse_copy (111-276): MAX_N_READ bytes from the current offsets;
   offload first when allowed, then buffered with optional hole scan.
   Returns bytes-copied (>= 0) or -1. *last_write_made_hole reports
   the trailing state for the caller's fixup. */
static intmax_t
sparse_copy(int src_fd, int dest_fd, size_t bsize,
            intmax_t max_n_read, bool hole_detection, bool allow_offload,
            const char *src_name, const char *dst_name,
            struct chopin_copy_debug *debug, bool *last_write_made_hole)
{
    intmax_t total_n_read = 0;

    *last_write_made_hole = false;

#if CHOPIN_HAVE_COPY_FILE_RANGE
    if (!hole_detection && allow_offload) {
        static bool force_scalar_checked;
        static bool force_scalar;

        if (!force_scalar_checked) {
            const char *e = getenv("CHOPIN_FORCE_SCALAR");
            force_scalar = e != NULL && *e != '\0' && *e != '0';
            force_scalar_checked = true;
        }
        while (!force_scalar && max_n_read > 0) {
            size_t chunk = max_n_read > ((ssize_t)SSIZE_MAX >> 30 << 30)
                ? (size_t)((ssize_t)SSIZE_MAX >> 30 << 30)
                : (size_t)max_n_read;
            ssize_t n = copy_file_range(src_fd, NULL, dest_fd, NULL,
                                        chunk, 0);

            if (n == 0) {
                /* /proc reports zero: fall back if nothing read yet;
                   else genuinely done. */
                if (total_n_read == 0)
                    break;
                debug->offload = "yes";
                return total_n_read;
            }
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EFBIG)
                    break;      /* mid-stream carve-out (149-155) */
                if (total_n_read == 0
                    && (errno == ENOSYS || errno == ENOTTY
                        || errno == ENOTSUP || errno == EOPNOTSUPP
                        || errno == EINVAL || errno == EBADF
                        || errno == EXDEV || errno == ETXTBSY
                        || errno == EPERM || errno == EACCES
                        || errno == ENOENT)) {
                    debug->offload = "unsupported";
                    break;
                }
                chopin_error(errno, "error copying %s to %s",
                             chopin_quoteaf_n(0, src_name),
                             chopin_quoteaf_n(1, dst_name));
                return -1;
            }
            total_n_read += n;
            max_n_read -= n;
            debug->offload = "yes";
        }
        if (max_n_read == 0)
            return total_n_read;
    }
#endif

    char *b = get_buffer(bsize);
    intmax_t psize = 0;         /* pending hole (or write) run */
    bool make_hole = false;

    while (max_n_read > 0) {
        size_t req = max_n_read > (intmax_t)bsize
            ? bsize : (size_t)max_n_read;
        ssize_t n_read = read(src_fd, b, req);

        if (n_read < 0) {
            if (errno == EINTR)
                continue;
            chopin_error(errno, "error reading %s",
                         chopin_quoteaf(src_name));
            return -1;
        }
        if (n_read == 0)
            break;
        max_n_read -= n_read;
        total_n_read += n_read;

        const char *p = b;
        size_t left = (size_t)n_read;

        while (left > 0) {
            size_t csize = left > NBLOCKSIZE ? NBLOCKSIZE : left;
            bool csize_hole = hole_detection && is_nul(p, csize);

            if (psize == 0)
                make_hole = csize_hole;
            if (csize_hole == make_hole) {
                if (psize > INTMAX_MAX - (intmax_t)csize) {
                    chopin_error(0, "overflow reading %s",
                                 chopin_quoteaf(src_name));
                    return -1;
                }
                psize += (intmax_t)csize;
            } else {
                if (make_hole) {
                    if (create_hole(dest_fd, dst_name, psize) < 0)
                        return -1;      /* DEV-005: GNU returns 0 */
                } else {
                    if (!full_write(dest_fd, dst_name,
                                    p - (size_t)psize
                                        + 0, /* run start below */
                                    (size_t)psize))
                        return -1;
                }
                make_hole = csize_hole;
                psize = (intmax_t)csize;
            }
            p += csize;
            left -= csize;
        }

        /* Flush the buffer's pending run before the next read refills
           the buffer (run pointers cannot cross buffers). */
        if (psize > 0) {
            if (make_hole) {
                if (create_hole(dest_fd, dst_name, psize) < 0)
                    return -1;
            } else {
                if (!full_write(dest_fd, dst_name,
                                p - (size_t)psize, (size_t)psize))
                    return -1;
            }
            *last_write_made_hole = make_hole;
            psize = 0;
        }
        if (hole_detection)
            debug->sparse = "zeros";
    }
    return total_n_read;
}

bool
chopin_copy_file_data(int src_fd, const struct stat *src_sb,
                      const char *src_name, int dest_fd,
                      const struct stat *dst_sb, const char *dst_name,
                      const struct chopin_options *x,
                      struct chopin_copy_debug *debug)
{
    size_t out_size = io_blksize(dst_sb);
    size_t in_size = io_blksize(src_sb);
    size_t blcm = buffer_lcm(in_size, out_size);
    size_t bsize = out_size;
    off_t data_start = 0;

    if (S_ISREG(src_sb->st_mode) && src_sb->st_size >= 0
        && (uintmax_t)src_sb->st_size + 1 < bsize)
        bsize = (size_t)src_sb->st_size + 1;
    bsize += blcm - 1;
    bsize -= bsize % blcm;
    if (bsize == 0)
        bsize = blcm;

    /* Entering the data path downgrades its stages from "unknown". */
    debug->offload = "no";
    debug->sparse = "no";

    /* Scantype is inferred (and reported) regardless of sparse mode:
       GNU says "sparse detection: SEEK_HOLE" even under
       --sparse=never. */
    enum scantype scantype = infer_scantype(src_fd, src_sb, &data_start);

    if (scantype == ERROR_SCANTYPE) {
        chopin_error(errno, "cannot lseek %s", chopin_quoteaf(src_name));
        return false;
    }
    bool make_holes = S_ISREG(dst_sb->st_mode)
        && (x->sparse_mode == CHOPIN_SPARSE_ALWAYS
            || (x->sparse_mode == CHOPIN_SPARSE_AUTO
                && scantype != PLAIN_SCANTYPE));
    /* --reflink=never disables offload too (quirk: allow_reflink). */
    bool allow_offload = x->reflink_mode != CHOPIN_REFLINK_NEVER;

    if (scantype == LSEEK_SCANTYPE)
        debug->sparse = "SEEK_HOLE";
    bool hole_detection = make_holes
        && (x->sparse_mode == CHOPIN_SPARSE_ALWAYS
            || scantype == ZERO_SCANTYPE);
    if (hole_detection && scantype == LSEEK_SCANTYPE)
        debug->sparse = "SEEK_HOLE + zeros";
    if (hole_detection && scantype == ZERO_SCANTYPE)
        debug->sparse = "zeros";

    /* Offload "avoided" = disabled by policy (reflink=never) or by
       active zero-detection, before any attempt. */
    if (!allow_offload || hole_detection)
        debug->offload = "avoided";

    intmax_t total = 0;
    bool last_hole = false;

#if CHOPIN_HAVE_SEEK_HOLE
    if (scantype == LSEEK_SCANTYPE && make_holes) {
        /* lseek_copy (326-451): walk extents. */
        off_t pos = 0;
        off_t size = src_sb->st_size;
        bool zero_scan = x->sparse_mode == CHOPIN_SPARSE_ALWAYS;

        for (;;) {
            if (data_start > pos) {
                intmax_t hole = data_start - pos;
                if (create_hole(dest_fd, dst_name, hole) < 0)
                    return false;
                last_hole = true;
            }
            if (data_start >= size)
                break;
            off_t hole_start = lseek(src_fd, data_start, SEEK_HOLE);
            if (hole_start < 0) {
                chopin_error(errno, "cannot lseek %s",
                             chopin_quoteaf(src_name));
                return false;
            }
            if (lseek(src_fd, data_start, SEEK_SET) < 0) {
                chopin_error(errno, "cannot lseek %s",
                             chopin_quoteaf(src_name));
                return false;
            }
            intmax_t ext = hole_start - data_start;
            /* Extents offload in auto mode (GNU reports
               "offload: yes" for SEEK_HOLE copies); zero-scan
               extents cannot. */
            intmax_t n = sparse_copy(src_fd, dest_fd, bsize, ext,
                                     zero_scan,
                                     allow_offload && !zero_scan,
                                     src_name, dst_name, debug,
                                     &last_hole);
            if (n < 0)
                return false;
            total += n;
            pos = data_start + n;
            if (n < ext)
                break;          /* source shrank mid-copy */
            off_t next = lseek(src_fd, pos, SEEK_DATA);
            if (next < 0) {
                if (errno == ENXIO) {
                    data_start = size > pos ? size : pos;
                    if (data_start > pos) {
                        if (create_hole(dest_fd, dst_name,
                                        data_start - pos) < 0)
                            return false;
                        last_hole = true;
                        pos = data_start;
                    }
                    break;
                }
                chopin_error(errno, "cannot lseek %s",
                             chopin_quoteaf(src_name));
                return false;
            }
            data_start = next;
        }

        /* Trailing hole: fix the size then punch (624-642). */
        off_t opos = lseek(dest_fd, 0, SEEK_CUR);
        if (last_hole && opos >= 0) {
            if (ftruncate(dest_fd, opos) < 0) {
                chopin_error(errno, "failed to extend %s",
                             chopin_quoteaf(dst_name));
                return false;
            }
        }
        return true;
    }
#endif

    intmax_t n = sparse_copy(src_fd, dest_fd, bsize, INTMAX_MAX,
                             hole_detection, allow_offload, src_name,
                             dst_name, debug, &last_hole);
    if (n < 0)
        return false;
    total = n;
    (void)total;

    if (last_hole) {
        off_t opos = lseek(dest_fd, 0, SEEK_CUR);
        if (opos < 0 || ftruncate(dest_fd, opos) < 0) {
            chopin_error(errno, "failed to extend %s",
                         chopin_quoteaf(dst_name));
            return false;
        }
    }
    return true;
}
