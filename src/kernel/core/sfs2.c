#include "sfs2.h"

#include <stddef.h>

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
