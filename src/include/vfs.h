#ifndef VEXOS_VFS_H
#define VEXOS_VFS_H

#include <stdint.h>

/* Initial VFS contract. The first mounted backend exposes a flat root
   namespace; nested directories and mount points are added in later steps. */
typedef struct {
    uint32_t size;
    uint8_t is_directory;
} vfs_stat_t;

int vfs_init(void);
int vfs_is_mounted(void);
int vfs_stat(const char *path, vfs_stat_t *out);
int vfs_file_size(const char *path, uint32_t *out_size);
int vfs_read_at(const char *path, uint32_t offset, void *out,
                uint32_t maxlen, uint32_t *out_len);
int vfs_write_file(const char *path, const void *data, uint32_t len);
uint32_t vfs_root_file_count(void);
int vfs_root_file_name_at(uint32_t index, char *out_name, uint32_t out_cap);

#endif
