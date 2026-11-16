#include "vfs.h"
#include "simplefs.h"
#include <stddef.h>
#include <stdint.h>

/*
 * VFS v2 increment: keep SFS1 on disk, but use slash-separated relative names
 * for files. Directories are implicit (a directory exists while it has at
 * least one descendant); empty directories are not persisted. The backend
 * remains private so consumers depend on VFS operations rather than SimpleFS.
 */

#define VFS_NODE_CACHE_MAX 64u

typedef struct {
    int (*mount)(void);
    int (*stat)(const char *name, vfs_stat_t *out);
    int (*read_at)(const char *name, uint32_t offset, void *out,
                   uint32_t max_len, uint32_t *out_len);
    int (*read)(const char *name, void *out, uint32_t max_len,
                uint32_t *out_len);
    int (*write)(const char *name, const void *data, uint32_t len);
    int (*unlink)(const char *name);
    uint32_t (*count)(void);
    int (*name_at)(uint32_t index, char *out, uint32_t cap);
    uint32_t (*list)(char *out, uint32_t cap);
} vfs_backend_ops_t;

typedef struct {
    const char *mount_path;
    const vfs_backend_ops_t *ops;
    uint8_t mounted;
} vfs_mount_t;

struct vfs_node {
    char path[VFS_PATH_MAX];
    uint32_t size;
    uint32_t references;
    uint8_t used;
    uint8_t busy;
    vfs_node_type_t type;
};

static struct vfs_node g_nodes[VFS_NODE_CACHE_MAX];
static volatile uint32_t g_node_lock;
static volatile uint32_t g_backend_lock;
/* Serialize namespace mutations across validation and backend commit. */
static volatile uint32_t g_namespace_lock;
static volatile uint32_t g_mount_lock;

/* VFS is called by preemptively scheduled tasks on a single CPU. Disable
   local interrupts before taking a lock so PIT preemption cannot switch to a
   second task that spins forever on a lock held by the preempted task. */
static uint64_t vfs_lock(volatile uint32_t *lock) {
    uint64_t irq_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(irq_flags) :: "memory");
    while (__sync_lock_test_and_set(lock, 1u)) {
        __asm__ volatile("pause");
    }
    return irq_flags;
}

static void vfs_unlock(volatile uint32_t *lock, uint64_t irq_flags) {
    __sync_lock_release(lock);
    if (irq_flags & (1ull << 9))
        __asm__ volatile("sti" ::: "memory");
}

#define VFS_LOCK(lock) uint64_t lock##_irq_flags = vfs_lock(&(lock))
#define VFS_UNLOCK(lock) vfs_unlock(&(lock), lock##_irq_flags)

static int vfs_string_equal(const char *a, const char *b) {
    if (!a || !b)
        return 0;
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i])
            return 0;
        ++i;
    }
    return a[i] == b[i];
}

static uint32_t vfs_string_length(const char *s) {
    uint32_t n = 0;
    if (!s)
        return 0;
    while (n < VFS_PATH_MAX && s[n])
        ++n;
    return n;
}

static void vfs_string_copy(char *dst, const char *src, uint32_t cap) {
    if (!dst || cap == 0)
        return;
    uint32_t i = 0;
    if (src) {
        while (i + 1u < cap && src[i]) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

static int simplefs_backend_mount(void) {
    return simplefs_init();
}

static int simplefs_backend_stat(const char *name, vfs_stat_t *out) {
    uint32_t size = 0;
    if (!name || !out)
        return 0;
    if (simplefs_is_directory(name)) {
        out->type = VFS_NODE_DIRECTORY;
        out->size = 0;
        return 1;
    }
    if (!simplefs_file_size(name, &size))
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

static int simplefs_backend_unlink(const char *name) {
    return simplefs_delete_file(name);
}

static const vfs_backend_ops_t g_simplefs_ops = {
    .mount = simplefs_backend_mount,
    .stat = simplefs_backend_stat,
    .read_at = simplefs_backend_read_at,
    .read = simplefs_backend_read,
    .write = simplefs_backend_write,
    .unlink = simplefs_backend_unlink,
    .count = simplefs_file_count,
    .name_at = simplefs_file_name_at,
    .list = simplefs_list_to_buffer,
};

static vfs_mount_t g_root_mount = {
    .mount_path = "/",
    .ops = &g_simplefs_ops,
    .mounted = 0,
};

/* Backend calls are serialized because SimpleFS owns one mutable global table
   and allocator. These are task-context operations, never interrupt handlers. */
static int vfs_backend_stat(const char *name, vfs_stat_t *out) {
    int result;
    VFS_LOCK(g_backend_lock);
    result = g_root_mount.ops->stat(name, out);
    VFS_UNLOCK(g_backend_lock);
    return result;
}

static int vfs_backend_read_at(const char *name, uint32_t offset, void *out,
                               uint32_t max_len, uint32_t *out_len) {
    int result;
    VFS_LOCK(g_backend_lock);
    result = g_root_mount.ops->read_at(name, offset, out, max_len, out_len);
    VFS_UNLOCK(g_backend_lock);
    return result;
}

static int vfs_backend_read(const char *name, void *out, uint32_t max_len,
                            uint32_t *out_len) {
    int result;
    VFS_LOCK(g_backend_lock);
    result = g_root_mount.ops->read(name, out, max_len, out_len);
    VFS_UNLOCK(g_backend_lock);
    return result;
}

static int vfs_backend_write(const char *name, const void *data, uint32_t len) {
    int result;
    VFS_LOCK(g_backend_lock);
    result = g_root_mount.ops->write(name, data, len);
    VFS_UNLOCK(g_backend_lock);
    return result;
}

static int vfs_backend_unlink(const char *name) {
    int result;
    if (!g_root_mount.ops->unlink)
        return 0;
    VFS_LOCK(g_backend_lock);
    result = g_root_mount.ops->unlink(name);
    VFS_UNLOCK(g_backend_lock);
    return result;
}

static uint32_t vfs_backend_count(void) {
    uint32_t result;
    VFS_LOCK(g_backend_lock);
    result = g_root_mount.ops->count ? g_root_mount.ops->count() : 0;
    VFS_UNLOCK(g_backend_lock);
    return result;
}

static int vfs_backend_name_at(uint32_t index, char *out, uint32_t cap) {
    int result;
    VFS_LOCK(g_backend_lock);
    result = g_root_mount.ops->name_at &&
             g_root_mount.ops->name_at(index, out, cap);
    VFS_UNLOCK(g_backend_lock);
    return result;
}

static uint32_t vfs_backend_list(char *out, uint32_t cap) {
    uint32_t result;
    VFS_LOCK(g_backend_lock);
    result = g_root_mount.ops->list ? g_root_mount.ops->list(out, cap) : 0;
    VFS_UNLOCK(g_backend_lock);
    return result;
}

int vfs_init(void) {
    int result = 0;
    VFS_LOCK(g_mount_lock);
    if (g_root_mount.mounted) {
        result = 1;
    } else if (g_root_mount.ops && g_root_mount.ops->mount) {
        VFS_LOCK(g_backend_lock);
        result = g_root_mount.ops->mount();
        VFS_UNLOCK(g_backend_lock);
        if (result)
            g_root_mount.mounted = 1;
    }
    VFS_UNLOCK(g_mount_lock);
    return result;
}

int vfs_is_mounted(void) {
    return g_root_mount.mounted != 0;
}

int vfs_normalize_path(const char *path, char *out, uint32_t out_cap) {
    if (!path || !out || out_cap == 0 || !vfs_init())
        return 0;

    uint32_t len = vfs_string_length(path);
    if (len == 0 || len >= VFS_PATH_MAX) {
        out[0] = '\0';
        return 0;
    }

    uint32_t src = 0;
    while (path[src] == '/')
        ++src;

    if (src == len) {
        if (out_cap < 2)
            return 0;
        out[0] = '/';
        out[1] = '\0';
        return 1;
    }

    uint32_t written = 0;
    uint32_t path_limit = simplefs_path_limit();
    while (src < len) {
        uint32_t component_start = src;
        while (src < len && path[src] != '/')
            ++src;
        uint32_t component_len = src - component_start;

        if (component_len == 0 ||
            (component_len == 1 && path[component_start] == '.')) {
            /* Collapse repeated separators and current-directory components. */
        } else if (component_len == 2 &&
                   path[component_start] == '.' &&
                   path[component_start + 1] == '.') {
            out[0] = '\0';
            return 0;
        } else {
            uint32_t separator = written ? 1u : 0u;
            if (component_len == 0 || component_len >= VFS_NAME_MAX ||
                written + separator + component_len > path_limit ||
                written + separator + component_len + 1u > out_cap) {
                out[0] = '\0';
                return 0;
            }
            if (separator)
                out[written++] = '/';
            for (uint32_t i = 0; i < component_len; ++i)
                out[written++] = path[component_start + i];
        }

        while (src < len && path[src] == '/')
            ++src;
    }

    if (written == 0) {
        if (out_cap < 2u)
            return 0;
        out[0] = '/';
        out[1] = '\0';
        return 1;
    }
    if (written >= out_cap) {
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

static int vfs_has_descendant(const char *directory) {
    uint32_t dir_len = (directory && !vfs_string_equal(directory, "/"))
                           ? vfs_string_length(directory) : 0;
    uint32_t count = vfs_backend_count();
    for (uint32_t i = 0; i < count; ++i) {
        char entry[VFS_PATH_MAX];
        if (!vfs_backend_name_at(i, entry, sizeof(entry)))
            continue;
        if (dir_len == 0) {
            if (entry[0])
                return 1;
        } else {
            uint32_t j = 0;
            while (j < dir_len && entry[j] &&
                   entry[j] == directory[j])
                ++j;
            if (j == dir_len && entry[dir_len] == '/' &&
                entry[dir_len + 1u] != '\0')
                return 1;
        }
    }
    return 0;
}

static int vfs_path_parent_is_file(const char *name) {
    char parent[VFS_PATH_MAX];
    uint32_t len = vfs_string_length(name);
    for (uint32_t i = 0; i < len; ++i) {
        if (name[i] != '/')
            continue;
        for (uint32_t j = 0; j < i; ++j)
            parent[j] = name[j];
        parent[i] = '\0';
        vfs_stat_t info;
        if (vfs_backend_stat(parent, &info))
            return 1;
    }
    return 0;
}

static struct vfs_node *vfs_node_find_locked(const char *path) {
    for (uint32_t i = 0; i < VFS_NODE_CACHE_MAX; ++i) {
        if (g_nodes[i].used && vfs_string_equal(g_nodes[i].path, path))
            return &g_nodes[i];
    }
    return NULL;
}

static struct vfs_node *vfs_node_acquire(const char *path,
                                         const vfs_stat_t *info) {
    struct vfs_node *node = NULL;
    VFS_LOCK(g_node_lock);
    node = vfs_node_find_locked(path);
    if (node) {
        if (node->busy || node->references == UINT32_MAX) {
            node = NULL;
        } else {
            ++node->references;
            node->size = info->size;
            node->type = info->type;
        }
    } else {
        for (uint32_t i = 0; i < VFS_NODE_CACHE_MAX; ++i) {
            if (!g_nodes[i].used) {
                node = &g_nodes[i];
                vfs_string_copy(node->path, path, sizeof(node->path));
                node->size = info->size;
                node->type = info->type;
                node->references = 1;
                node->busy = 0;
                node->used = 1;
                break;
            }
        }
    }
    VFS_UNLOCK(g_node_lock);
    return node;
}

static void vfs_node_release(struct vfs_node *node) {
    if (!node)
        return;
    VFS_LOCK(g_node_lock);
    if (node->used && node->references > 0) {
        --node->references;
        if (node->references == 0 && !node->busy) {
            node->used = 0;
            node->path[0] = '\0';
            node->size = 0;
            node->type = 0;
        }
    }
    VFS_UNLOCK(g_node_lock);
}

static struct vfs_node *vfs_node_reserve_path(const char *path) {
    struct vfs_node *node = NULL;
    VFS_LOCK(g_node_lock);
    node = vfs_node_find_locked(path);
    if (node) {
        if (node->busy || node->references != 0)
            node = NULL;
        else
            node->busy = 1;
    } else {
        for (uint32_t i = 0; i < VFS_NODE_CACHE_MAX; ++i) {
            if (!g_nodes[i].used) {
                node = &g_nodes[i];
                vfs_string_copy(node->path, path, sizeof(node->path));
                node->size = 0;
                node->type = VFS_NODE_FILE;
                node->references = 0;
                node->busy = 1;
                node->used = 1;
                break;
            }
        }
    }
    VFS_UNLOCK(g_node_lock);
    return node;
}

static void vfs_node_release_reservation(struct vfs_node *node) {
    if (!node)
        return;
    VFS_LOCK(g_node_lock);
    if (node->used && node->busy && node->references == 0) {
        node->used = 0;
        node->busy = 0;
        node->path[0] = '\0';
        node->size = 0;
        node->type = 0;
    }
    VFS_UNLOCK(g_node_lock);
}

int vfs_stat(const char *path, vfs_stat_t *out_stat) {
    if (!out_stat || !vfs_init() || !g_root_mount.ops)
        return 0;

    char name[VFS_PATH_MAX];
    if (!vfs_resolve(path, name, sizeof(name)))
        return 0;

    if (vfs_string_equal(name, "/")) {
        out_stat->type = VFS_NODE_DIRECTORY;
        out_stat->size = 0;
        return 1;
    }

    if (vfs_backend_stat(name, out_stat))
        return 1;

    if (vfs_has_descendant(name)) {
        out_stat->type = VFS_NODE_DIRECTORY;
        out_stat->size = 0;
        return 1;
    }
    return 0;
}

int vfs_read_at(const char *path, uint32_t offset, void *out,
                uint32_t max_len, uint32_t *out_len) {
    if (out_len)
        *out_len = 0;
    if ((!out && max_len != 0) || !vfs_init() ||
        !g_root_mount.ops || !g_root_mount.ops->read_at)
        return 0;
    char name[VFS_PATH_MAX];
    vfs_stat_t info;
    if (!vfs_resolve(path, name, sizeof(name)) ||
        !vfs_backend_stat(name, &info) || info.type != VFS_NODE_FILE)
        return 0;
    return vfs_backend_read_at(name, offset, out, max_len, out_len);
}

int vfs_read(const char *path, void *out, uint32_t max_len,
             uint32_t *out_len) {
    if (out_len)
        *out_len = 0;
    if ((!out && max_len != 0) || !vfs_init() ||
        !g_root_mount.ops || !g_root_mount.ops->read)
        return 0;
    char name[VFS_PATH_MAX];
    vfs_stat_t info;
    if (!vfs_resolve(path, name, sizeof(name)) ||
        !vfs_backend_stat(name, &info) || info.type != VFS_NODE_FILE)
        return 0;
    return vfs_backend_read(name, out, max_len, out_len);
}

int vfs_write(const char *path, const void *data, uint32_t len) {
    if ((!data && len != 0) || !vfs_init() ||
        !g_root_mount.ops || !g_root_mount.ops->write)
        return 0;
    char name[VFS_PATH_MAX];
    if (!vfs_resolve(path, name, sizeof(name)))
        return 0;

    /* Keep parent/type checks, reservation, and backend update atomic with
       respect to other namespace mutations. Otherwise two tasks could race
       to create both "node" and "node/child". */
    VFS_LOCK(g_namespace_lock);
    if (vfs_string_equal(name, "/") || vfs_path_parent_is_file(name) ||
        vfs_has_descendant(name)) {
        VFS_UNLOCK(g_namespace_lock);
        return 0;
    }

    struct vfs_node *reservation = vfs_node_reserve_path(name);
    if (!reservation) {
        VFS_UNLOCK(g_namespace_lock);
        return 0;
    }
    static const uint8_t empty = 0;
    int result = vfs_backend_write(name, data ? data : &empty, len);
    vfs_node_release_reservation(reservation);
    VFS_UNLOCK(g_namespace_lock);
    return result;
}

int vfs_create(const char *path) {
    if (!vfs_init() || !g_root_mount.ops || !g_root_mount.ops->write)
        return 0;
    char name[VFS_PATH_MAX];
    if (!vfs_resolve(path, name, sizeof(name)))
        return 0;

    VFS_LOCK(g_namespace_lock);
    vfs_stat_t info;
    if (vfs_string_equal(name, "/") || vfs_path_parent_is_file(name) ||
        vfs_backend_stat(name, &info) || vfs_has_descendant(name)) {
        VFS_UNLOCK(g_namespace_lock);
        return 0;
    }

    struct vfs_node *reservation = vfs_node_reserve_path(name);
    if (!reservation) {
        VFS_UNLOCK(g_namespace_lock);
        return 0;
    }
    static const uint8_t empty = 0;
    int result = vfs_backend_write(name, &empty, 0);
    vfs_node_release_reservation(reservation);
    VFS_UNLOCK(g_namespace_lock);
    return result;
}

int vfs_unlink(const char *path) {
    if (!vfs_init() || !g_root_mount.ops || !g_root_mount.ops->unlink)
        return 0;
    char name[VFS_PATH_MAX];
    if (!vfs_resolve(path, name, sizeof(name)) ||
        vfs_string_equal(name, "/"))
        return 0;

    VFS_LOCK(g_namespace_lock);
    struct vfs_node *reservation = vfs_node_reserve_path(name);
    if (!reservation) {
        VFS_UNLOCK(g_namespace_lock);
        return 0;
    }
    vfs_stat_t info;
    if (!vfs_backend_stat(name, &info) || info.type != VFS_NODE_FILE) {
        vfs_node_release_reservation(reservation);
        VFS_UNLOCK(g_namespace_lock);
        return 0;
    }
    int result = vfs_backend_unlink(name);
    vfs_node_release_reservation(reservation);
    VFS_UNLOCK(g_namespace_lock);
    return result;
}

int vfs_open(vfs_file_t *file, const char *path) {
    if (!file || file->is_open || !vfs_init())
        return 0;

    char name[VFS_PATH_MAX];
    vfs_stat_t info;
    if (!vfs_resolve(path, name, sizeof(name)) ||
        !vfs_backend_stat(name, &info) || info.type != VFS_NODE_FILE)
        return 0;

    struct vfs_node *node = vfs_node_acquire(name, &info);
    if (!node)
        return 0;

    /* Close the lookup/unlink race: once a reference exists, unlink/write
       cannot reserve the path. Confirm the backing entry still exists. */
    if (!vfs_backend_stat(name, &info) || info.type != VFS_NODE_FILE) {
        vfs_node_release(node);
        return 0;
    }
    node->size = info.size;
    vfs_string_copy(file->path, name, sizeof(file->path));
    file->size = info.size;
    file->offset = 0;
    file->node = node;
    file->is_open = 1;
    return 1;
}

uint32_t vfs_file_remaining(const vfs_file_t *file) {
    if (!file || !file->is_open || !file->node || !file->node->used)
        return 0;
    uint32_t size = file->node->size;
    return file->offset < size ? size - file->offset : 0;
}

int32_t vfs_file_read(vfs_file_t *file, void *out, uint32_t count) {
    if (!file || !file->is_open || !file->node ||
        (!out && count != 0))
        return -1;
    uint32_t available = vfs_file_remaining(file);
    if (count == 0 || available == 0)
        return 0;

    uint32_t wanted = count < available ? count : available;
    uint32_t actual = 0;
    if (!vfs_backend_read_at(file->node->path, file->offset,
                             out, wanted, &actual) || actual > wanted)
        return -1;
    file->offset += actual;
    return (int32_t)actual;
}

int64_t vfs_file_seek(vfs_file_t *file, int64_t offset, int whence) {
    if (!file || !file->is_open || !file->node)
        return -1;

    int64_t size = file->node->size;
    int64_t base;
    if (whence == 0)
        base = 0;
    else if (whence == 1)
        base = file->offset;
    else if (whence == 2)
        base = size;
    else
        return -1;

    if (offset < -base || offset > size - base)
        return -1;
    int64_t target = base + offset;
    file->offset = (uint32_t)target;
    file->size = (uint32_t)size;
    return target;
}

int vfs_close(vfs_file_t *file) {
    if (!file || !file->is_open || !file->node)
        return 0;
    struct vfs_node *node = file->node;
    file->path[0] = '\0';
    file->size = 0;
    file->offset = 0;
    file->node = NULL;
    file->is_open = 0;
    vfs_node_release(node);
    return 1;
}

static int vfs_child_from_entry(const char *directory, const char *entry,
                                char *child, uint32_t child_cap,
                                int *is_directory) {
    uint32_t dir_len = (!directory || vfs_string_equal(directory, "/"))
                           ? 0u : vfs_string_length(directory);
    uint32_t entry_len = vfs_string_length(entry);
    const char *remainder = entry;

    if (dir_len) {
        uint32_t i = 0;
        while (i < dir_len && entry[i] == directory[i])
            ++i;
        if (i != dir_len || entry[dir_len] != '/')
            return 0;
        remainder = entry + dir_len + 1u;
    }

    if (!remainder[0])
        return 0;

    uint32_t child_len = 0;
    while (remainder[child_len] && remainder[child_len] != '/')
        ++child_len;
    if (child_len == 0 || child_len >= child_cap)
        return -1;

    for (uint32_t i = 0; i < child_len; ++i)
        child[i] = remainder[i];
    child[child_len] = '\0';
    if (is_directory)
        *is_directory = remainder[child_len] == '/';
    (void)entry_len;
    return 1;
}

static int vfs_child_seen_before(const vfs_dir_t *dir, uint32_t index,
                                 const char *child) {
    for (uint32_t i = 0; i < index; ++i) {
        char previous[VFS_NAME_MAX];
        int previous_is_dir = 0;
        char entry[VFS_PATH_MAX];
        if (!vfs_backend_name_at(i, entry, sizeof(entry)))
            continue;
        int result = vfs_child_from_entry(dir->path, entry, previous,
                                          sizeof(previous), &previous_is_dir);
        if (result == 1 && vfs_string_equal(previous, child))
            return 1;
    }
    return 0;
}

int vfs_dir_open(vfs_dir_t *dir, const char *path) {
    if (!dir || dir->is_open || !vfs_init())
        return 0;
    char normalized[VFS_PATH_MAX];
    vfs_stat_t info;
    if (!vfs_normalize_path(path, normalized, sizeof(normalized)) ||
        !vfs_stat(normalized, &info) || info.type != VFS_NODE_DIRECTORY)
        return 0;
    vfs_string_copy(dir->path, normalized, sizeof(dir->path));
    dir->next_index = 0;
    dir->is_open = 1;
    return 1;
}

int vfs_dir_read(vfs_dir_t *dir, char *out_name, uint32_t out_cap,
                 vfs_stat_t *out_stat) {
    if (!dir || !dir->is_open || !out_name || out_cap == 0 || !out_stat)
        return -1;

    uint32_t count = vfs_backend_count();
    while (dir->next_index < count) {
        uint32_t index = dir->next_index++;
        char entry[VFS_PATH_MAX];
        if (!vfs_backend_name_at(index, entry, sizeof(entry)))
            continue;

        char child[VFS_NAME_MAX];
        int is_directory = 0;
        int result = vfs_child_from_entry(dir->path, entry, child,
                                          sizeof(child), &is_directory);
        if (result <= 0)
            continue;

        char full_path[VFS_PATH_MAX];
        uint32_t dir_len = vfs_string_equal(dir->path, "/")
                               ? 0u : vfs_string_length(dir->path);
        uint32_t child_len = vfs_string_length(child);
        uint32_t needed = dir_len + (dir_len ? 1u : 0u) + child_len;
        if (needed >= sizeof(full_path))
            continue;
        uint32_t pos = 0;
        if (dir_len) {
            for (uint32_t i = 0; i < dir_len; ++i)
                full_path[pos++] = dir->path[i];
            full_path[pos++] = '/';
        }
        for (uint32_t i = 0; i < child_len; ++i)
            full_path[pos++] = child[i];
        full_path[pos] = '\0';

        vfs_stat_t exact_info;
        int exact_exists = vfs_backend_stat(full_path, &exact_info);
        int exact_file = exact_exists && exact_info.type == VFS_NODE_FILE;
        int exact_directory = exact_exists &&
                              exact_info.type == VFS_NODE_DIRECTORY;
        if (is_directory || exact_directory) {
            if (exact_file || vfs_child_seen_before(dir, index, child))
                continue;
            out_stat->type = VFS_NODE_DIRECTORY;
            out_stat->size = 0;
        } else {
            if (!exact_file)
                continue;
            *out_stat = exact_info;
        }

        if (child_len + 1u > out_cap) {
            /* Do not consume the entry: callers can retry with a larger buffer. */
            --dir->next_index;
            return -1;
        }
        for (uint32_t i = 0; i <= child_len; ++i)
            out_name[i] = child[i];
        return 1;
    }
    return 0;
}

int vfs_dir_close(vfs_dir_t *dir) {
    if (!dir || !dir->is_open)
        return 0;
    dir->path[0] = '\0';
    dir->next_index = 0;
    dir->is_open = 0;
    return 1;
}

uint32_t vfs_file_count(void) {
    return vfs_init() && g_root_mount.ops && g_root_mount.ops->count
               ? vfs_backend_count() : 0;
}

int vfs_file_name_at(uint32_t index, char *out_name, uint32_t out_cap) {
    return vfs_init() && g_root_mount.ops &&
           g_root_mount.ops->name_at &&
           vfs_backend_name_at(index, out_name, out_cap);
}

uint32_t vfs_list_to_buffer(char *out, uint32_t out_cap) {
    return vfs_init() && g_root_mount.ops && g_root_mount.ops->list
               ? vfs_backend_list(out, out_cap) : 0;
}
