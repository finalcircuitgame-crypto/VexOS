#include "vfs.h"
#include "simplefs.h"
#include <stddef.h>
#include <stdint.h>

/*
 * VFS v1: a small mount/backend boundary over the existing SimpleFS API.
 * The disk format is deliberately unchanged. The backend is kept private so
 * consumers depend on paths and VFS operations, not SimpleFS implementation
 * details. Directory trees and additional mounts arrive in later milestones.
 */
typedef struct {
    int (*mount)(void);
    int (*stat)(const char *name, vfs_stat_t *out);
    int (*read_at)(const char *name, uint32_t offset, void *out,
                   uint32_t max_len, uint32_t *out_len);
    int (*read)(const char *name, void *out, uint32_t max_len,
                uint32_t *out_len);
    int (*write)(const char *name, const void *data, uint32_t len);
    uint32_t (*count)(void);
    int (*name_at)(uint32_t index, char *out, uint32_t cap);
    uint32_t (*list)(char *out, uint32_t cap);
} vfs_backend_ops_t;

typedef struct {
    const char *mount_path;
    const vfs_backend_ops_t *ops;
    uint8_t mounted;
} vfs_mount_t;

static int simplefs_backend_mount(void) {
    return simplefs_init();
}

static int simplefs_backend_stat(const char *name, vfs_stat_t *out) {
    uint32_t size = 0;
    if (!name || !out || !simplefs_file_size(name, &size))
        return 0;
    out->type = VFS_NODE_FILE;
    out->size = size;
    return 1;
}

static int simplefs_backend_read_at(const char *name, uint32_t offset,
                                    void *out, uint32_t max_len,
                                    uint32_t *out_len) {
    return simplefs_read_file_at(name, offset, out, max_len, out_len);
}

static int simplefs_backend_read(const char *name, void *out,
                                 uint32_t max_len, uint32_t *out_len) {
    return simplefs_read_file(name, out, max_len, out_len);
}

static int simplefs_backend_write(const char *name, const void *data,
                                  uint32_t len) {
    return simplefs_write_file(name, data, len);
}

static const vfs_backend_ops_t g_simplefs_ops = {
    .mount = simplefs_backend_mount,
    .stat = simplefs_backend_stat,
    .read_at = simplefs_backend_read_at,
    .read = simplefs_backend_read,
    .write = simplefs_backend_write,
    .count = simplefs_file_count,
    .name_at = simplefs_file_name_at,
    .list = simplefs_list_to_buffer,
};

static vfs_mount_t g_root_mount = {
    .mount_path = "/",
    .ops = &g_simplefs_ops,
    .mounted = 0,
};

static uint32_t path_length(const char *s) {
    uint32_t n = 0;
    if (!s)
        return 0;
    while (s[n] && n < VFS_PATH_MAX)
        ++n;
    return n;
}

int vfs_init(void) {
    if (g_root_mount.mounted)
        return 1;
    if (!g_root_mount.ops || !g_root_mount.ops->mount ||
        !g_root_mount.ops->mount())
        return 0;
    g_root_mount.mounted = 1;
    return 1;
}

int vfs_is_mounted(void) {
    return g_root_mount.mounted != 0;
}

int vfs_normalize_path(const char *path, char *out, uint32_t out_cap) {
    if (!path || !out || out_cap == 0 || !g_root_mount.mounted)
        return 0;

    uint32_t len = path_length(path);
    if (len == 0 || len >= VFS_PATH_MAX)
        return 0;

    uint32_t src = 0;
    while (path[src] == '/')
        ++src;

    /* Root is a directory; the flat backend has no child directories. */
    if (src == len) {
        out[0] = '/';
        if (out_cap < 2)
            return 0;
        out[1] = '\0';
        return 1;
    }

    uint32_t written = 0;
    while (src < len) {
        uint32_t component_start = src;
        while (src < len && path[src] != '/')
            ++src;
        uint32_t component_len = src - component_start;

        if (component_len == 0 ||
            (component_len == 1 && path[component_start] == '.')) {
            /* Repeated separators and current-directory components. */
        } else if (component_len == 2 &&
                   path[component_start] == '.' &&
                   path[component_start + 1] == '.') {
            out[0] = '\0';
            return 0; /* No parent traversal in the flat root backend. */
        } else {
            /* The current on-disk backend stores root files only. */
            if (written != 0 || component_len >= out_cap ||
                component_len > 31u) {
                out[0] = '\0';
                return 0;
            }
            for (uint32_t i = 0; i < component_len; ++i)
                out[written++] = path[component_start + i];
        }

        while (src < len && path[src] == '/')
            ++src;
    }

    if (written == 0 || written >= out_cap) {
        out[0] = '\0';
        return 0;
    }
    out[written] = '\0';
    return 1;
}

static int vfs_resolve(const char *path, char *name, uint32_t cap) {
    return g_root_mount.mounted && g_root_mount.ops &&
           vfs_normalize_path(path, name, cap);
}

int vfs_stat(const char *path, vfs_stat_t *out_stat) {
    if (!out_stat || !g_root_mount.mounted || !g_root_mount.ops)
        return 0;
    if (path && path[0] == '/' && path[1] == '\0') {
        out_stat->type = VFS_NODE_DIRECTORY;
        out_stat->size = 0;
        return 1;
    }
    char name[VFS_PATH_MAX];
    if (!vfs_resolve(path, name, sizeof(name)))
        return 0;
    return g_root_mount.ops->stat(name, out_stat);
}

int vfs_read_at(const char *path, uint32_t offset, void *out,
                uint32_t max_len, uint32_t *out_len) {
    if (out_len)
        *out_len = 0;
    if ((!out && max_len != 0) || !g_root_mount.ops ||
        !g_root_mount.ops->read_at)
        return 0;
    char name[VFS_PATH_MAX];
    if (!vfs_resolve(path, name, sizeof(name)))
        return 0;
    return g_root_mount.ops->read_at(name, offset, out, max_len, out_len);
}

int vfs_read(const char *path, void *out, uint32_t max_len,
             uint32_t *out_len) {
    if (out_len)
        *out_len = 0;
    if ((!out && max_len != 0) || !g_root_mount.ops ||
        !g_root_mount.ops->read)
        return 0;
    char name[VFS_PATH_MAX];
    if (!vfs_resolve(path, name, sizeof(name)))
        return 0;
    return g_root_mount.ops->read(name, out, max_len, out_len);
}

int vfs_write(const char *path, const void *data, uint32_t len) {
    if ((!data && len != 0) || !g_root_mount.ops ||
        !g_root_mount.ops->write)
        return 0;
    char name[VFS_PATH_MAX];
    if (!vfs_resolve(path, name, sizeof(name)))
        return 0;
    /* SimpleFS expects a non-null pointer for an empty file. */
    static const uint8_t empty = 0;
    return g_root_mount.ops->write(name, data ? data : &empty, len);
}

uint32_t vfs_file_count(void) {
    return g_root_mount.mounted && g_root_mount.ops && g_root_mount.ops->count
               ? g_root_mount.ops->count() : 0;
}

int vfs_file_name_at(uint32_t index, char *out_name, uint32_t out_cap) {
    return g_root_mount.mounted && g_root_mount.ops &&
           g_root_mount.ops->name_at &&
           g_root_mount.ops->name_at(index, out_name, out_cap);
}

uint32_t vfs_list_to_buffer(char *out, uint32_t out_cap) {
    return g_root_mount.mounted && g_root_mount.ops && g_root_mount.ops->list
               ? g_root_mount.ops->list(out, out_cap) : 0;
}
