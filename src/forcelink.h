#ifndef CHOPIN_FORCELINK_H
#define CHOPIN_FORCELINK_H

#include <stdbool.h>

/* force-link.c protocol: -1 replaced an existing dest, 0 created
   fresh, positive errno on failure. */
int chopin_force_linkat(int srcdir, const char *srcname,
                        int dstdir, const char *dstname, int flags,
                        bool force);
int chopin_force_symlinkat(const char *target, int dstdir,
                           const char *dstname, bool force);

#endif
