#ifndef CHOPIN_COPYDATA_H
#define CHOPIN_COPYDATA_H

#include <stdbool.h>
#include <sys/stat.h>

/* The scalar read/write engine (sprint 02). The engine ladder
   (reflink, copy_file_range, sparse walkers) lands in sprint 07 with
   this loop as its verification oracle. */
bool chopin_copy_file_data(int src_fd, const struct stat *src_sb,
                           const char *src_name, int dest_fd,
                           const struct stat *dst_sb, const char *dst_name);

#endif
