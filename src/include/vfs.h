#ifndef VEXOS_VFS_H
#define VEXOS_VFS_H

#include <stdint.h>

#define VFS_PATH_MAX 128u

typedef enum {
    VFS_NODE_FILE = 1,
    VFS_NODE_DIRECTORY = 2
} vfs_node_type_t;

typedef struct {
    vfs_node_type_t type;
    uint32_t size;
} vfs_stat_t;

/* Per-open handle. Independent opens have independent offsets. */
typedef struct {
    char path[VFS_PATH_MAX];
    uint32_t size;
    uint32_t offset;
    uint8_t is_open;
} vfs_file_t;

/* Mount the existing persistent filesystem at /. Safe to call repeatedly. */
int vfs_init(void);
int vfs_is_mounted(void);

/* Normalize a root-relative or absolute path into the current backend name.
   The initial SimpleFS backend is flat, so nested paths are rejected. */
int vfs_normalize_path(const char *path, char *out, uint32_t out_cap);

int vfs_stat(const char *path, vfs_stat_t *out_stat);
int vfs_read_at(const char *path, uint32_t offset, void *out,
                uint32_t max_len, uint32_t *out_len);
int vfs_read(const char *path, void *out, uint32_t max_len,
             uint32_t *out_len);
int vfs_write(const char *path, const void *data, uint32_t len);

int vfs_open(vfs_file_t *file, const char *path);
int32_t vfs_file_read(vfs_file_t *file, void *out, uint32_t count);
int64_t vfs_file_seek(vfs_file_t *file, int64_t offset, int whence);
int vfs_close(vfs_file_t *file);
uint32_t vfs_file_count(void);
int vfs_file_name_at(uint32_t index, char *out_name, uint32_t out_cap);
uint32_t vfs_list_to_buffer(char *out, uint32_t out_cap);

#endif
