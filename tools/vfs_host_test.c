#include "vfs.h"
#include "simplefs.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FAKE_FILES 64u
#define FAKE_PATH 128u
#define FAKE_DATA 4096u

typedef struct {
    char name[FAKE_PATH];
    uint8_t data[FAKE_DATA];
    uint32_t size;
    uint8_t used;
} fake_file_t;

static fake_file_t files[FAKE_FILES];
static int mounted;

static void fail(const char *message) {
    fprintf(stderr, "VFS TEST FAILED: %s\n", message);
}

#define CHECK(expr, message) do { if (!(expr)) { fail(message); return 1; } } while (0)

static int find_file(const char *name) {
    for (uint32_t i = 0; i < FAKE_FILES; ++i)
        if (files[i].used && strcmp(files[i].name, name) == 0)
            return (int)i;
    return -1;
}

int simplefs_init(void) { mounted = 1; return 1; }
int simplefs_is_mounted(void) { return mounted; }
uint32_t simplefs_path_limit(void) { return 127u; }

int simplefs_is_directory(const char *name) {
    if (!name || !name[0]) return 0;
    size_t n = strlen(name);
    for (uint32_t i = 0; i < FAKE_FILES; ++i) {
        if (!files[i].used) continue;
        if (strncmp(files[i].name, name, n) == 0 &&
            files[i].name[n] == '/')
            return 1;
    }
    return 0;
}

int simplefs_file_size(const char *name, uint32_t *out_size) {
    int i = find_file(name);
    if (i < 0 || !out_size) return 0;
    *out_size = files[i].size;
    return 1;
}

int simplefs_read_file_at(const char *name, uint32_t offset, void *out,
                          uint32_t maxlen, uint32_t *out_len) {
    int i = find_file(name);
    if (out_len) *out_len = 0;
    if (i < 0 || (!out && maxlen)) return 0;
    uint32_t amount = offset < files[i].size ? files[i].size - offset : 0;
    if (amount > maxlen) amount = maxlen;
    if (amount) memcpy(out, files[i].data + offset, amount);
    if (out_len) *out_len = amount;
    return 1;
}

int simplefs_read_file(const char *name, void *out, uint32_t maxlen,
                       uint32_t *out_len) {
    return simplefs_read_file_at(name, 0, out, maxlen, out_len);
}

int simplefs_write_file(const char *name, const void *data, uint32_t len) {
    if (!name || !name[0] || len > FAKE_DATA || (!data && len)) return 0;
    int i = find_file(name);
    if (i < 0) {
        for (uint32_t j = 0; j < FAKE_FILES; ++j)
            if (!files[j].used) { i = (int)j; break; }
    }
    if (i < 0) return 0;
    size_t name_len = strlen(name);
    if (name_len >= sizeof(files[i].name)) return 0;
    memset(&files[i], 0, sizeof(files[i]));
    memcpy(files[i].name, name, name_len + 1u);
    if (len) memcpy(files[i].data, data, len);
    files[i].size = len;
    files[i].used = 1;
    return 1;
}

int simplefs_delete_file(const char *name) {
    int i = find_file(name);
    if (i < 0) return 0;
    memset(&files[i], 0, sizeof(files[i]));
    return 1;
}

uint32_t simplefs_file_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < FAKE_FILES; ++i) n += files[i].used != 0;
    return n;
}

int simplefs_file_name_at(uint32_t index, char *out_name, uint32_t out_cap) {
    if (!out_name || !out_cap) return 0;
    for (uint32_t i = 0; i < FAKE_FILES; ++i) {
        if (!files[i].used) continue;
        if (index-- == 0) {
            size_t n = strlen(files[i].name);
            if (n + 1u > out_cap) return 0;
            memcpy(out_name, files[i].name, n + 1u);
            return 1;
        }
    }
    return 0;
}

uint32_t simplefs_list_to_buffer(char *out, uint32_t out_cap) {
    uint32_t used = 0;
    if (!out || !out_cap) return 0;
    for (uint32_t i = 0; i < simplefs_file_count(); ++i) {
        char name[FAKE_PATH];
        if (!simplefs_file_name_at(i, name, sizeof(name))) continue;
        size_t n = strlen(name);
        if (used + n + 1u > out_cap) break;
        memcpy(out + used, name, n);
        used += (uint32_t)n;
        out[used++] = '\n';
    }
    return used;
}

int main(void) {
    char normalized[VFS_PATH_MAX];
    CHECK(vfs_init(), "mount succeeds");
    CHECK(vfs_init() && vfs_is_mounted(), "mount is idempotent");
    CHECK(vfs_normalize_path("//docs///./guide.txt", normalized,
                             sizeof(normalized)), "normalize valid path");
    CHECK(strcmp(normalized, "docs/guide.txt") == 0, "normalized path value");
    CHECK(!vfs_normalize_path("../escape", normalized, sizeof(normalized)),
          "reject parent traversal");
    CHECK(!vfs_normalize_path("docs/../../escape", normalized,
                              sizeof(normalized)), "reject nested traversal");

    static const char original[] = "filesystem runtime";
    CHECK(vfs_write("/docs/guide.txt", original, sizeof(original)),
          "write nested file");
    vfs_stat_t info;
    CHECK(vfs_stat("/", &info) && info.type == VFS_NODE_DIRECTORY,
          "stat root directory");
    CHECK(vfs_stat("docs", &info) && info.type == VFS_NODE_DIRECTORY,
          "implicit directory lookup");
    CHECK(vfs_stat("docs/guide.txt", &info) &&
          info.type == VFS_NODE_FILE && info.size == sizeof(original),
          "stat file metadata");

    vfs_file_t first = {0}, second = {0};
    CHECK(vfs_open(&first, "docs/guide.txt"), "open first handle");
    CHECK(vfs_open(&second, "/docs/guide.txt"), "open second handle");
    char buffer[64] = {0};
    CHECK(vfs_file_read(&first, buffer, 4) == 4 &&
          memcmp(buffer, "file", 4) == 0, "read first handle");
    CHECK(vfs_file_read(&second, buffer, 5) == 5 &&
          memcmp(buffer, "files", 5) == 0, "independent file offsets");
    CHECK(vfs_file_seek(&first, -4, 2) == (int64_t)sizeof(original) - 4,
          "seek from end");
    memset(buffer, 0, sizeof(buffer));
    CHECK(vfs_file_read(&first, buffer, 4) == 4 &&
          memcmp(buffer, "time", 4) == 0, "read after seek");
    CHECK(vfs_file_seek(&first, -1, 0) == -1, "reject negative seek");
    CHECK(!vfs_write("docs/guide.txt", "replacement", 11),
          "reject replacement while open");
    CHECK(!vfs_unlink("docs/guide.txt"),
          "reject unlink while open");
    CHECK(vfs_close(&first) && vfs_close(&second), "close both handles");
    CHECK(vfs_write("docs/guide.txt", "updated", 7),
          "replace file after close");

    vfs_dir_t root = {0}, docs = {0};
    CHECK(vfs_dir_open(&root, "/"), "open root directory");
    char entry[ VFS_NAME_MAX ];
    CHECK(vfs_dir_read(&root, entry, 2, &info) == -1,
          "small directory buffer reports retryable error");
    CHECK(vfs_dir_read(&root, entry, sizeof(entry), &info) == 1 &&
          strcmp(entry, "docs") == 0 &&
          info.type == VFS_NODE_DIRECTORY, "root enumerates immediate directory");
    CHECK(vfs_dir_read(&root, entry, sizeof(entry), &info) == 0,
          "root does not enumerate grandchildren");
    CHECK(vfs_dir_close(&root), "close root iterator");
    CHECK(vfs_dir_open(&docs, "docs"), "open nested directory");
    CHECK(vfs_dir_read(&docs, entry, sizeof(entry), &info) == 1 &&
          strcmp(entry, "guide.txt") == 0 &&
          info.type == VFS_NODE_FILE, "nested iterator returns file");
    CHECK(vfs_dir_read(&docs, entry, sizeof(entry), &info) == 0,
          "nested iterator reaches end");
    CHECK(vfs_dir_close(&docs), "close nested iterator");

    CHECK(vfs_unlink("docs/guide.txt"), "unlink file");
    CHECK(!vfs_stat("docs", &info), "implicit directory disappears when empty");
    CHECK(!vfs_open(&first, "docs/guide.txt"), "cannot reopen unlinked file");
    CHECK(vfs_create("empty.txt"), "create empty file");
    CHECK(vfs_stat("empty.txt", &info) && info.size == 0,
          "empty file has zero length");
    CHECK(vfs_unlink("empty.txt"), "unlink empty file");

    puts("[VFS] mount, normalization, traversal rejection, nested paths, stat,");
    puts("[VFS] read/write/create/unlink, independent handles, seek, open-file");
    puts("[VFS] protection, immediate-child iteration, and error cases passed.");
    return 0;
}
