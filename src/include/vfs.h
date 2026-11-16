#ifndef VEXOS_VFS_H
#define VEXOS_VFS_H

#include <stdint.h>

#define VFS_PATH_MAX 128u
#define VFS_NAME_MAX 32u

typedef enum {
    VFS_NODE_FILE = 1,
    VFS_NODE_DIRECTORY = 2
} vfs_node_type_t;

typedef struct {
    vfs_node_type_t type;
    uint32_t size;
} vfs_stat_t;

struct vfs_node;

/* Zero-initialize before open. Each handle has an independent offset. */
typedef struct {
    char path[VFS_PATH_MAX];
    uint32_t size;
    uint32_t offset;
    uint8_t is_open;
    struct vfs_node *node;
} vfs_file_t;

/* Zero-initialize before open. Iterators enumerate immediate children. */
typedef struct {
    char path[VFS_PATH_MAX];
    uint32_t next_index;
    uint8_t is_open;
} vfs_dir_t;

/* Mount the existing persistent filesystem at /. Safe to call repeatedly. */
int vfs_init(void);
int vfs_is_mounted(void);

/* Normalize absolute or root-relative paths. Parent traversal is rejected.
   The SFS1 backend stores at most 31 bytes total, including '/' separators. */
int vfs_normalize_path(const char *path, char *out, uint32_t out_cap);

int vfs_stat(const char *path, vfs_stat_t *out_stat);
int vfs_read_at(const char *path, uint32_t offset, void *out,
                uint32_t max_len, uint32_t *out_len);
int vfs_read(const char *path, void *out, uint32_t max_len,
             uint32_t *out_len);

/* Write creates or replaces a file; it fails for directories or open files. */
int vfs_write(const char *path, const void *data, uint32_t len);
/* Create an empty file. Directories are implicit and appear with first child. */
int vfs_create(const char *path);
/* Unlink a file. Open files cannot be unlinked. SFS1 sectors are not reclaimed. */
int vfs_unlink(const char *path);

int vfs_open(vfs_file_t *file, const char *path);
int32_t vfs_file_read(vfs_file_t *file, void *out, uint32_t count);
uint32_t vfs_file_remaining(const vfs_file_t *file);
int64_t vfs_file_seek(vfs_file_t *file, int64_t offset, int whence);
int vfs_close(vfs_file_t *file);

int vfs_dir_open(vfs_dir_t *dir, const char *path);
/* Returns 1 for an entry, 0 at end, and -1 for an invalid handle/buffer. */
int vfs_dir_read(vfs_dir_t *dir, char *out_name, uint32_t out_cap,
                 vfs_stat_t *out_stat);
int vfs_dir_close(vfs_dir_t *dir);

/* Legacy raw-backend enumeration; prefer vfs_dir_open/read for UI consumers. */
uint32_t vfs_file_count(void);
int vfs_file_name_at(uint32_t index, char *out_name, uint32_t out_cap);
uint32_t vfs_list_to_buffer(char *out, uint32_t out_cap);

#endif
