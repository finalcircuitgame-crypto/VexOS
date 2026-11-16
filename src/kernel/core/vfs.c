#include "vfs.h"
#include "simplefs.h"
#include <stddef.h>
#include <stdint.h>

#define VFS_PATH_MAX 128u
#define VFS_NAME_MAX 32u

static uint8_t g_vfs_mounted;

static int vfs_normalize_root_file(const char *path, char out[VFS_NAME_MAX]) {
    if (!path || !out)
        return 0;

    uint32_t i = 0;
    while (path[i] == '/')
        ++i;

    /* Accept the common relative spelling used by existing applications. */
    while (path[i] == '.' && path[i + 1u] == '/') {
        i += 2u;
        while (path[i] == '/')
            ++i;
    }

    if (!path[i])
        return 0; /* Root is a directory, not a regular file. */

    uint32_t n = 0;
    while (path[i] && path[i] != '/') {
        if (n + 1u >= VFS_NAME_MAX)
            return 0;
        if (path[i] == '\\' || path[i] == ':')
            return 0;
        out[n++] = path[i++];
    }

    /* The initial SimpleFS backend is flat. Reject nested paths instead
       of silently mapping /a/b to a different object. */
    if (path[i] != '\0' || n == 0)
        return 0;

    out[n] = '\0';
    return 1;
}

int vfs_init(void) {
    if (g_vfs_mounted)
        return 1;

    if (!simplefs_is_mounted() && !simplefs_init())
        return 0;

    g_vfs_mounted = (uint8_t)simplefs_is_mounted();
    return g_vfs_mounted != 0;
}

int vfs_is_mounted(void) {
    if (!g_vfs_mounted)
        return vfs_init();
    return simplefs_is_mounted();
}

int vfs_stat(const char *path, vfs_stat_t *out) {
    char name[VFS_NAME_MAX];
    uint32_t size = 0;

    if (!vfs_is_mounted() || !vfs_normalize_root_file(path, name) ||
        !simplefs_file_size(name, &size))
        return 0;

    if (out) {
        out->size = size;
        out->is_directory = 0;
    }
    return 1;
}

int vfs_file_size(const char *path, uint32_t *out_size) {
    vfs_stat_t st;
    if (!vfs_stat(path, &st))
        return 0;
    if (out_size)
        *out_size = st.size;
    return 1;
}

int vfs_read_at(const char *path, uint32_t offset, void *out,
                uint32_t maxlen, uint32_t *out_len) {
    char name[VFS_NAME_MAX];
    if (out_len)
        *out_len = 0;
    if (!vfs_is_mounted() || !vfs_normalize_root_file(path, name))
        return 0;
    return simplefs_read_file_at(name, offset, out, maxlen, out_len);
}

int vfs_write_file(const char *path, const void *data, uint32_t len) {
    char name[VFS_NAME_MAX];
    if (!vfs_is_mounted() || !vfs_normalize_root_file(path, name))
        return 0;
    return simplefs_write_file(name, data, len);
}

uint32_t vfs_root_file_count(void) {
    return vfs_is_mounted() ? simplefs_file_count() : 0;
}

int vfs_root_file_name_at(uint32_t index, char *out_name, uint32_t out_cap) {
    if (!vfs_is_mounted())
        return 0;
    return simplefs_file_name_at(index, out_name, out_cap);
}
