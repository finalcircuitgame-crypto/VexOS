#include "sfs2.h"

#include <stddef.h>

_Static_assert(sizeof(sfs2_superblock_t) == SFS2_SECTOR_SIZE,
               "SFS2 superblock must occupy exactly one sector");
_Static_assert(sizeof(sfs2_inode_t) == 96u,
               "SFS2 inode record size is part of the disk format");

static void sfs2_zero(void *ptr, uint32_t length) {
    uint8_t *bytes = (uint8_t *)ptr;
    for (uint32_t i = 0; i < length; ++i)
        bytes[i] = 0;
}

uint32_t sfs2_checksum(const void *data, uint32_t length,
                       uint32_t checksum_offset) {
    if (!data || checksum_offset > length ||
        length - checksum_offset < sizeof(uint32_t))
        return 0;

    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < length; ++i) {
        uint8_t value = (i >= checksum_offset &&
                         i < checksum_offset + sizeof(uint32_t))
                            ? 0u : bytes[i];
        hash ^= value;
        hash *= 16777619u;
    }
    return hash;
}

int sfs2_validate_superblock(const sfs2_superblock_t *sb,
                             uint64_t device_sectors) {
    if (!sb || device_sectors == 0 ||
        sb->magic != SFS2_MAGIC || sb->version != SFS2_VERSION ||
        sb->header_size != sizeof(sfs2_superblock_t) ||
        sb->sector_size != SFS2_SECTOR_SIZE ||
        sb->volume_sectors == 0 || sb->volume_sectors > device_sectors ||
        sb->inode_capacity == 0 || sb->inode_capacity > SFS2_INODE_MAX ||
        sb->root_inode == 0)
        return 0;

    uint64_t bitmap_end = sb->bitmap_lba + sb->bitmap_sectors;
    uint64_t inode_end = sb->inode_table_lba + sb->inode_table_sectors;
    if (sb->bitmap_lba < 1u || sb->bitmap_sectors == 0 ||
        bitmap_end < sb->bitmap_lba || bitmap_end > sb->volume_sectors ||
        sb->inode_table_lba < bitmap_end || sb->inode_table_sectors == 0 ||
        inode_end < sb->inode_table_lba || inode_end > sb->volume_sectors ||
        sb->data_lba < inode_end || sb->data_lba >= sb->volume_sectors)
        return 0;

    uint64_t bitmap_bits = (uint64_t)sb->bitmap_sectors *
                           SFS2_SECTOR_SIZE * 8u;
    uint64_t inode_bytes = (uint64_t)sb->inode_table_sectors *
                           SFS2_SECTOR_SIZE;
    if (bitmap_bits < sb->volume_sectors - sb->data_lba ||
        inode_bytes < (uint64_t)sb->inode_capacity * sizeof(sfs2_inode_t))
        return 0;

    return sb->checksum ==
           sfs2_checksum(sb, sizeof(*sb),
                         (uint32_t)offsetof(sfs2_superblock_t, checksum));
}

int sfs2_validate_inode(const sfs2_inode_t *inode,
                        uint64_t data_lba, uint64_t volume_sectors) {
    if (!inode || inode->inode_id == 0 ||
        (inode->type != SFS2_INODE_FILE &&
         inode->type != SFS2_INODE_DIRECTORY) ||
        inode->checksum !=
            sfs2_checksum(inode, sizeof(*inode),
                          (uint32_t)offsetof(sfs2_inode_t, checksum)))
        return 0;

    /* Inode 1 is the unique root directory and has no parent or name. */
    if (inode->inode_id == 1u)
        return inode->type == SFS2_INODE_DIRECTORY &&
               inode->parent_id == 0u && inode->name[0] == '\\0' &&
               inode->size_bytes == 0 && inode->data_lba == 0 &&
               inode->data_sectors == 0;

    if (inode->parent_id == 0u)
        return 0;

    uint32_t name_length = 0;
    while (name_length < SFS2_NAME_MAX && inode->name[name_length])
        ++name_length;
    if (name_length == 0 || name_length >= SFS2_NAME_MAX ||
        inode->name[0] == '/' ||
        (name_length == 1u && inode->name[0] == '.') ||
        (name_length == 2u && inode->name[0] == '.' &&
         inode->name[1] == '.'))
        return 0;
    for (uint32_t i = 0; i < name_length; ++i)
        if (inode->name[i] == '/')
            return 0;

    if (inode->type == SFS2_INODE_DIRECTORY)
        return inode->size_bytes == 0 && inode->data_lba == 0 &&
               inode->data_sectors == 0;

    if (inode->size_bytes == 0)
        return inode->data_sectors == 0 && inode->data_lba == 0;

    uint64_t required = inode->size_bytes / SFS2_SECTOR_SIZE +
                        ((inode->size_bytes % SFS2_SECTOR_SIZE) != 0u);
    uint64_t end = inode->data_lba + inode->data_sectors;
    if (inode->data_lba < data_lba || inode->data_sectors < required ||
        end < inode->data_lba || end > volume_sectors)
        return 0;
    return 1;
}

int sfs2_bitmap_test(const uint8_t *bitmap, uint32_t bit_count,
                     uint32_t bit_index) {
    if (!bitmap || bit_index >= bit_count)
        return -1;
    return (bitmap[bit_index >> 3] >> (bit_index & 7u)) & 1u;
}

int sfs2_bitmap_find_free_run(const uint8_t *bitmap, uint32_t bit_count,
                              uint32_t run_length, uint32_t *out_start) {
    if (!bitmap || !out_start || run_length == 0 || run_length > bit_count)
        return 0;

    uint32_t run = 0;
    for (uint32_t i = 0; i < bit_count; ++i) {
        if (!sfs2_bitmap_test(bitmap, bit_count, i)) {
            if (++run == run_length) {
                *out_start = i + 1u - run_length;
                return 1;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}


static int sfs2_read_inode_table(sfs2_volume_t *volume) {
    uint8_t sector[SFS2_SECTOR_SIZE];
    uint32_t inode_index = 0;
    uint32_t byte_offset = 0;
    uint8_t record[sizeof(sfs2_inode_t)];
    uint32_t record_bytes = 0;
    uint64_t lba = volume->superblock.inode_table_lba;
    for (uint32_t s = 0; s < volume->superblock.inode_table_sectors; ++s) {
        if (!volume->read_sector(lba + s, sector))
            return 0;
        for (uint32_t i = 0; i < SFS2_SECTOR_SIZE; ++i, ++byte_offset) {
            record[record_bytes++] = sector[i];
            if (record_bytes == sizeof(sfs2_inode_t)) {
                if (inode_index >= volume->superblock.inode_capacity)
                    return 0;
                uint8_t *dst = (uint8_t *)&volume->inodes[inode_index++];
                for (uint32_t j = 0; j < sizeof(sfs2_inode_t); ++j)
                    dst[j] = record[j];
                record_bytes = 0;
            }
        }
    }
    return inode_index >= volume->superblock.inode_capacity &&
           record_bytes == 0;
}

static int sfs2_find_inode_id(const sfs2_volume_t *volume, uint32_t id,
                              uint32_t *out_index) {
    if (!volume || !volume->mounted || id == 0)
        return 0;
    for (uint32_t i = 0; i < volume->superblock.inode_capacity; ++i) {
        if (volume->inodes[i].inode_id == id) {
            if (out_index)
                *out_index = i;
            return 1;
        }
    }
    return 0;
}

static int sfs2_name_equal(const char *a, const char *b) {
    if (!a || !b)
        return 0;
    uint32_t i = 0;
    while (a[i] && b[i] && a[i] == b[i])
        ++i;
    return a[i] == b[i];
}

static int sfs2_validate_namespace(sfs2_volume_t *volume) {
    uint32_t root_count = 0;
    for (uint32_t i = 0; i < volume->superblock.inode_capacity; ++i) {
        sfs2_inode_t *inode = &volume->inodes[i];
        if (inode->inode_id == 0)
            continue;
        if (!sfs2_validate_inode(inode, volume->superblock.data_lba,
                                 volume->superblock.volume_sectors))
            return 0;
        if (inode->inode_id == volume->superblock.root_inode) {
            if (inode->type != SFS2_INODE_DIRECTORY)
                return 0;
            ++root_count;
        } else {
            uint32_t parent_index = 0;
            if (!sfs2_find_inode_id(volume, inode->parent_id, &parent_index) ||
                volume->inodes[parent_index].type != SFS2_INODE_DIRECTORY)
                return 0;
        }
        for (uint32_t j = 0; j < i; ++j) {
            const sfs2_inode_t *other = &volume->inodes[j];
            if (other->inode_id == 0)
                continue;
            if (other->inode_id == inode->inode_id)
                return 0;
            if (other->parent_id == inode->parent_id &&
                inode->inode_id != volume->superblock.root_inode &&
                other->inode_id != volume->superblock.root_inode &&
                sfs2_name_equal(other->name, inode->name))
                return 0;
        }
    }
    return root_count == 1u;
}

int sfs2_mount(sfs2_volume_t *volume, sfs2_read_sector_fn read_sector,
               sfs2_write_sector_fn write_sector, uint64_t device_sectors) {
    if (!volume || !read_sector || device_sectors == 0)
        return 0;
    volume->mounted = 0;
    volume->read_sector = read_sector;
    volume->write_sector = write_sector;
    uint8_t sector[SFS2_SECTOR_SIZE];
    if (!read_sector(0, sector))
        return 0;
    const uint8_t *src = sector;
    uint8_t *dst = (uint8_t *)&volume->superblock;
    for (uint32_t i = 0; i < sizeof(volume->superblock); ++i)
        dst[i] = src[i];
    if (!sfs2_validate_superblock(&volume->superblock, device_sectors))
        return 0;
    for (uint32_t i = 0; i < SFS2_INODE_MAX; ++i)
        sfs2_zero(&volume->inodes[i], sizeof(sfs2_inode_t));
    if (!sfs2_read_inode_table(volume))
        return 0;
    /* Temporarily mark mounted so parent-ID lookups can validate the tree. */
    volume->mounted = 1;
    if (!sfs2_validate_namespace(volume)) {
        volume->mounted = 0;
        return 0;
    }
    return 1;
}

static int sfs2_lookup_child(const sfs2_volume_t *volume, uint32_t parent_id,
                             const char *name, uint32_t name_length,
                             uint32_t *out_index) {
    for (uint32_t i = 0; i < volume->superblock.inode_capacity; ++i) {
        const sfs2_inode_t *inode = &volume->inodes[i];
        if (!inode->inode_id || inode->parent_id != parent_id)
            continue;
        uint32_t n = 0;
        while (n < SFS2_NAME_MAX && inode->name[n])
            ++n;
        if (n != name_length)
            continue;
        uint32_t j = 0;
        while (j < n && inode->name[j] == name[j])
            ++j;
        if (j == n) {
            if (out_index)
                *out_index = i;
            return 1;
        }
    }
    return 0;
}

static int sfs2_resolve_path(const sfs2_volume_t *volume, const char *path,
                             uint32_t *out_index) {
    if (!volume || !volume->mounted || !path || !out_index)
        return 0;
    uint32_t current = 0;
    if (!sfs2_find_inode_id(volume, volume->superblock.root_inode, &current))
        return 0;
    uint32_t pos = 0;
    while (path[pos] == '/')
        ++pos;
    if (!path[pos]) {
        *out_index = current;
        return 1;
    }
    while (path[pos]) {
        uint32_t start = pos;
        while (path[pos] && path[pos] != '/')
            ++pos;
        uint32_t length = pos - start;
        if (!length || length >= SFS2_NAME_MAX ||
            (length == 1u && path[start] == '.') ||
            (length == 2u && path[start] == '.' && path[start + 1u] == '.'))
            return 0;
        if (volume->inodes[current].type != SFS2_INODE_DIRECTORY)
            return 0;
        uint32_t child = 0;
        if (!sfs2_lookup_child(volume, volume->inodes[current].inode_id,
                               path + start, length, &child))
            return 0;
        current = child;
        while (path[pos] == '/')
            ++pos;
        if (path[pos] && volume->inodes[current].type != SFS2_INODE_DIRECTORY)
            return 0;
    }
    *out_index = current;
    return 1;
}

int sfs2_stat_path(sfs2_volume_t *volume, const char *path,
                   sfs2_stat_t *out_stat) {
    uint32_t index = 0;
    if (!out_stat || !sfs2_resolve_path(volume, path, &index))
        return 0;
    const sfs2_inode_t *inode = &volume->inodes[index];
    out_stat->inode_id = inode->inode_id;
    out_stat->parent_id = inode->parent_id;
    out_stat->type = inode->type;
    out_stat->size_bytes = inode->size_bytes;
    return 1;
}

int sfs2_read_file_at(sfs2_volume_t *volume, const char *path,
                      uint64_t offset, void *out, uint32_t length,
                      uint32_t *out_read) {
    if (out_read)
        *out_read = 0;
    uint32_t index = 0;
    if ((!out && length) || !sfs2_resolve_path(volume, path, &index))
        return 0;
    const sfs2_inode_t *inode = &volume->inodes[index];
    if (inode->type != SFS2_INODE_FILE || offset > inode->size_bytes)
        return 0;
    uint64_t available = inode->size_bytes - offset;
    uint32_t wanted = available < length ? (uint32_t)available : length;
    uint8_t sector[SFS2_SECTOR_SIZE];
    uint8_t *destination = (uint8_t *)out;
    uint32_t copied = 0;
    while (copied < wanted) {
        uint64_t absolute = offset + copied;
        uint64_t lba = inode->data_lba + absolute / SFS2_SECTOR_SIZE;
        uint32_t within = (uint32_t)(absolute % SFS2_SECTOR_SIZE);
        uint32_t chunk = SFS2_SECTOR_SIZE - within;
        if (chunk > wanted - copied)
            chunk = wanted - copied;
        if (!volume->read_sector(lba, sector))
            return 0;
        for (uint32_t i = 0; i < chunk; ++i)
            destination[copied + i] = sector[within + i];
        copied += chunk;
    }
    if (out_read)
        *out_read = copied;
    return 1;
}

int sfs2_list_directory(sfs2_volume_t *volume, const char *path,
                        uint32_t *cursor, sfs2_inode_t *out_inode) {
    uint32_t directory_index = 0;
    if (!cursor || !out_inode ||
        !sfs2_resolve_path(volume, path, &directory_index) ||
        volume->inodes[directory_index].type != SFS2_INODE_DIRECTORY)
        return -1;
    uint32_t parent_id = volume->inodes[directory_index].inode_id;
    while (*cursor < volume->superblock.inode_capacity) {
        uint32_t index = (*cursor)++;
        if (volume->inodes[index].inode_id &&
            volume->inodes[index].parent_id == parent_id) {
            *out_inode = volume->inodes[index];
            return 1;
        }
    }
    return 0;
}


#ifdef VEXOS_CI_INPUT_TEST
static uint8_t g_sfs2_test_disk[16u][SFS2_SECTOR_SIZE];

static int sfs2_test_read(uint64_t lba, void *sector) {
    if (!sector || lba >= 16u)
        return 0;
    const uint8_t *src = g_sfs2_test_disk[lba];
    uint8_t *dst = (uint8_t *)sector;
    for (uint32_t i = 0; i < SFS2_SECTOR_SIZE; ++i)
        dst[i] = src[i];
    return 1;
}

static void sfs2_test_store_inode(uint32_t index,
                                  const sfs2_inode_t *inode) {
    uint32_t offset = index * (uint32_t)sizeof(*inode);
    const uint8_t *src = (const uint8_t *)inode;
    for (uint32_t i = 0; i < sizeof(*inode); ++i) {
        uint32_t absolute = offset + i;
        g_sfs2_test_disk[2u + absolute / SFS2_SECTOR_SIZE]
                        [absolute % SFS2_SECTOR_SIZE] = src[i];
    }
}

static void sfs2_test_finish_inode(sfs2_inode_t *inode) {
    inode->checksum = sfs2_checksum(inode, sizeof(*inode),
                                    (uint32_t)offsetof(sfs2_inode_t, checksum));
}

int sfs2_ci_mount_read_test(void) {
    sfs2_zero(g_sfs2_test_disk, sizeof(g_sfs2_test_disk));
    sfs2_superblock_t sb;
    sfs2_zero(&sb, sizeof(sb));
    sb.magic = SFS2_MAGIC;
    sb.version = SFS2_VERSION;
    sb.header_size = sizeof(sb);
    sb.sector_size = SFS2_SECTOR_SIZE;
    sb.volume_sectors = 16u;
    sb.generation = 1u;
    sb.bitmap_lba = 1u;
    sb.bitmap_sectors = 1u;
    sb.inode_table_lba = 2u;
    sb.inode_table_sectors = 1u;
    sb.inode_capacity = 4u;
    sb.data_lba = 3u;
    sb.root_inode = 1u;
    sb.checksum = sfs2_checksum(&sb, sizeof(sb),
                                (uint32_t)offsetof(sfs2_superblock_t, checksum));
    const uint8_t *sb_src = (const uint8_t *)&sb;
    for (uint32_t i = 0; i < sizeof(sb); ++i)
        g_sfs2_test_disk[0][i] = sb_src[i];

    sfs2_inode_t inode;
    sfs2_zero(&inode, sizeof(inode));
    inode.inode_id = 1u;
    inode.type = SFS2_INODE_DIRECTORY;
    sfs2_test_finish_inode(&inode);
    sfs2_test_store_inode(0u, &inode);

    sfs2_zero(&inode, sizeof(inode));
    inode.inode_id = 2u;
    inode.parent_id = 1u;
    inode.type = SFS2_INODE_DIRECTORY;
    inode.name[0] = 'e'; inode.name[1] = 'm'; inode.name[2] = 'p';
    inode.name[3] = 't'; inode.name[4] = 'y';
    sfs2_test_finish_inode(&inode);
    sfs2_test_store_inode(1u, &inode);

    sfs2_zero(&inode, sizeof(inode));
    inode.inode_id = 3u;
    inode.parent_id = 1u;
    inode.type = SFS2_INODE_FILE;
    inode.name[0] = 'h'; inode.name[1] = 'e'; inode.name[2] = 'l';
    inode.name[3] = 'l'; inode.name[4] = 'o';
    inode.size_bytes = 5u;
    inode.data_lba = 3u;
    inode.data_sectors = 1u;
    sfs2_test_finish_inode(&inode);
    sfs2_test_store_inode(2u, &inode);
    g_sfs2_test_disk[3][0] = 'v'; g_sfs2_test_disk[3][1] = 'e';
    g_sfs2_test_disk[3][2] = 'x'; g_sfs2_test_disk[3][3] = 'o';
    g_sfs2_test_disk[3][4] = 's';

    sfs2_volume_t volume;
    sfs2_zero(&volume, sizeof(volume));
    if (!sfs2_mount(&volume, sfs2_test_read, NULL, 16u))
        return 0;
    sfs2_stat_t info;
    if (!sfs2_stat_path(&volume, "/empty", &info) ||
        info.type != SFS2_INODE_DIRECTORY)
        return 0;
    char contents[5];
    uint32_t bytes = 0;
    if (!sfs2_read_file_at(&volume, "/hello", 0u, contents, 5u, &bytes) ||
        bytes != 5u || contents[0] != 'v' || contents[1] != 'e' ||
        contents[2] != 'x' || contents[3] != 'o' || contents[4] != 's')
        return 0;
    uint32_t cursor = 0;
    sfs2_inode_t listed;
    if (sfs2_list_directory(&volume, "/", &cursor, &listed) != 1)
        return 0;
    if (sfs2_stat_path(&volume, "/missing", &info))
        return 0;
    /* A damaged inode checksum must prevent mounting. */
    g_sfs2_test_disk[2][sizeof(sfs2_inode_t) + 12u] ^= 0x80u;
    volume.mounted = 0;
    if (sfs2_mount(&volume, sfs2_test_read, NULL, 16u))
        return 0;
    return 1;
}
#endif

#ifdef VEXOS_CI_INPUT_TEST
int sfs2_ci_metadata_self_test(void) {
    sfs2_superblock_t sb;
    sfs2_zero(&sb, sizeof(sb));
    sb.magic = SFS2_MAGIC;
    sb.version = SFS2_VERSION;
    sb.header_size = sizeof(sb);
    sb.sector_size = SFS2_SECTOR_SIZE;
    sb.volume_sectors = 4096;
    sb.generation = 1;
    sb.bitmap_lba = 1;
    sb.bitmap_sectors = 1;
    sb.inode_table_lba = 2;
    sb.inode_table_sectors = 48;
    sb.inode_capacity = SFS2_INODE_MAX;
    sb.data_lba = 50;
    sb.root_inode = 1;
    sb.checksum = sfs2_checksum(&sb, sizeof(sb),
                                (uint32_t)offsetof(sfs2_superblock_t,
                                                   checksum));
    if (!sfs2_validate_superblock(&sb, 4096))
        return 0;
    sb.data_lba = 1;
    if (sfs2_validate_superblock(&sb, 4096))
        return 0;

    sfs2_inode_t root;
    sfs2_zero(&root, sizeof(root));
    root.inode_id = 1u;
    root.type = SFS2_INODE_DIRECTORY;
    root.checksum = sfs2_checksum(&root, sizeof(root),
                                  (uint32_t)offsetof(sfs2_inode_t, checksum));
    if (!sfs2_validate_inode(&root, 50u, 4096u))
        return 0;
    root.parent_id = 1u;
    root.checksum = sfs2_checksum(&root, sizeof(root),
                                  (uint32_t)offsetof(sfs2_inode_t, checksum));
    if (sfs2_validate_inode(&root, 50u, 4096u))
        return 0;

    uint8_t bitmap[2] = {0xF0u, 0x00u};
    uint32_t start = 99;
    if (!sfs2_bitmap_find_free_run(bitmap, 16, 4, &start) || start != 4)
        return 0;
    if (sfs2_bitmap_find_free_run(bitmap, 16, 13, &start))
        return 0;
    if (sfs2_bitmap_test(bitmap, 16, 16) != -1)
        return 0;
    return 1;
}
#endif
