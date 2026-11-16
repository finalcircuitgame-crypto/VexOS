#ifndef VEXOS_USER_VEXFS_H
#define VEXOS_USER_VEXFS_H

#include <stdint.h>

#define VEXFS_NODE_FILE 1u
#define VEXFS_NODE_DIRECTORY 2u
#define VEXFS_NAME_MAX 32u

typedef struct {
    uint32_t type;
    uint32_t size;
} vexfs_stat_t;

typedef struct {
    int fd;
} vexfs_dir_t;

typedef struct {
    char name[VEXFS_NAME_MAX];
    uint32_t type;
    uint32_t size;
} vexfs_dirent_t;

static inline uint64_t vexfs_syscall0(uint64_t number) {
    uint64_t result;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(number)
                     : "memory", "rcx", "r11");
    return result;
}

static inline uint64_t vexfs_syscall1(uint64_t number, uint64_t a1) {
    uint64_t result;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(number), "D"(a1)
                     : "memory", "rcx", "r11");
    return result;
}

static inline uint64_t vexfs_syscall2(uint64_t number, uint64_t a1,
                                      uint64_t a2) {
    uint64_t result;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(number), "D"(a1), "S"(a2)
                     : "memory", "rcx", "r11");
    return result;
}

static inline uint64_t vexfs_syscall3(uint64_t number, uint64_t a1,
                                      uint64_t a2, uint64_t a3) {
    uint64_t result;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(number), "D"(a1), "S"(a2), "d"(a3)
                     : "memory", "rcx", "r11");
    return result;
}

/* These two calls return file size in RDX as a secondary result. */
static inline int64_t vexfs_syscall1_size(uint64_t number, uint64_t a1,
                                          uint64_t *out_size) {
    uint64_t result, size;
    __asm__ volatile("int $0x80"
                     : "=a"(result), "=d"(size)
                     : "a"(number), "D"(a1)
                     : "memory", "rcx", "r11");
    if (out_size)
        *out_size = size;
    return (int64_t)result;
}

static inline int64_t vexfs_syscall3_size(uint64_t number, uint64_t a1,
                                          uint64_t a2, uint64_t a3,
                                          uint64_t *out_size) {
    uint64_t result, size = a3;
    __asm__ volatile("int $0x80"
                     : "=a"(result), "+d"(size)
                     : "a"(number), "D"(a1), "S"(a2)
                     : "memory", "rcx", "r11");
    if (out_size)
        *out_size = size;
    return (int64_t)result;
}

/* File operations. Write replaces the whole file; there is no append mode. */
static inline int vexfs_create(const char *path) {
    return (int64_t)vexfs_syscall1(16u, (uint64_t)(uintptr_t)path);
}

static inline int vexfs_unlink(const char *path) {
    return (int64_t)vexfs_syscall1(17u, (uint64_t)(uintptr_t)path);
}

static inline int vexfs_write_file(const char *path, const void *data,
                                   uint32_t length) {
    return (int64_t)vexfs_syscall3(18u, (uint64_t)(uintptr_t)path,
                                   (uint64_t)(uintptr_t)data, length);
}

static inline int vexfs_stat(const char *path, vexfs_stat_t *out_stat) {
    if (!out_stat)
        return -1;
    uint64_t size = 0;
    int64_t type = vexfs_syscall1_size(
        19u, (uint64_t)(uintptr_t)path, &size);
    if (type < 0)
        return -1;
    out_stat->type = (uint32_t)type;
    out_stat->size = (uint32_t)size;
    return 0;
}

static inline int vexfs_open_read(const char *path) {
    return (int64_t)vexfs_syscall1(3u, (uint64_t)(uintptr_t)path);
}

static inline int vexfs_read_file(int fd, void *buffer, uint32_t length) {
    return (int64_t)vexfs_syscall3(4u, (uint64_t)fd,
                                   (uint64_t)(uintptr_t)buffer, length);
}

static inline int vexfs_seek_file(int fd, int32_t offset, int whence) {
    return (int64_t)vexfs_syscall3(6u, (uint64_t)fd,
                                   (uint64_t)(int64_t)offset,
                                   (uint64_t)whence);
}

static inline int vexfs_close_file(int fd) {
    return (int64_t)vexfs_syscall1(5u, (uint64_t)fd);
}

/* Directory handles share the process descriptor table with open files. */
static inline int vexfs_open_dir(vexfs_dir_t *directory, const char *path) {
    if (!directory)
        return -1;
    directory->fd = (int64_t)vexfs_syscall1(
        20u, (uint64_t)(uintptr_t)path);
    return directory->fd;
}

/* Returns 1 for a file, 2 for a directory, 0 at end, or -1 on error. */
static inline int vexfs_readdir_sized(vexfs_dir_t *directory, char *name,
                                      uint32_t capacity, uint32_t *out_size) {
    if (!directory || directory->fd < 3 || !name || capacity == 0 ||
        capacity > VEXFS_NAME_MAX)
        return -1;
    uint64_t size = 0;
    int64_t type = vexfs_syscall3_size(
        21u, (uint64_t)directory->fd, (uint64_t)(uintptr_t)name,
        capacity, &size);
    if (out_size)
        *out_size = (uint32_t)size;
    return (int)type;
}

static inline int vexfs_readdir(vexfs_dir_t *directory,
                                vexfs_dirent_t *entry) {
    if (!entry)
        return -1;
    uint32_t size = 0;
    int type = vexfs_readdir_sized(directory, entry->name,
                                   sizeof(entry->name), &size);
    if (type > 0) {
        entry->type = (uint32_t)type;
        entry->size = size;
    }
    return type;
}

static inline int vexfs_close_dir(vexfs_dir_t *directory) {
    if (!directory || directory->fd < 3)
        return -1;
    int result = (int64_t)vexfs_syscall1(5u, (uint64_t)directory->fd);
    if (result == 0)
        directory->fd = -1;
    return result;
}

#endif
