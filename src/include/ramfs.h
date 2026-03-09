#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>

int ramfs_init(void);
int ramfs_put_copy(const char *name, const void *data, uint32_t size);
int ramfs_get(const char *name, const void **out_data, uint32_t *out_size);

#endif
