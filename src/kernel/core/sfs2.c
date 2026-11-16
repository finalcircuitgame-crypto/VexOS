#include "sfs2.h"

#include <stddef.h>

_Static_assert(sizeof(sfs2_superblock_t) == SFS2_SECTOR_SIZE,
               "SFS2 superblock must occupy exactly one sector");
_Static_assert(sizeof(sfs2_inode_t) == 96u,
               "SFS2 inode record size is part of the disk format");

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t state;
    uint32_t record_count;
    uint32_t checksum;
    uint64_t sequence;
    uint64_t targets[SFS2_JOURNAL_MAX_RECORDS];
    uint32_t data_checksums[SFS2_JOURNAL_MAX_RECORDS];
    uint8_t reserved[104];
} sfs2_journal_header_t;

_Static_assert(sizeof(sfs2_journal_header_t) == SFS2_SECTOR_SIZE,
               "SFS2 journal header must occupy one sector");

static int sfs2_sector_is_zero(const uint8_t *sector) {
    for (uint32_t i = 0; i < SFS2_SECTOR_SIZE; ++i)
        if (sector[i] != 0)
            return 0;
    return 1;
}

static int sfs2_replay_journal(const sfs2_superblock_t *sb,
                               sfs2_read_sector_fn read_sector,
                               sfs2_write_sector_fn write_sector) {
    if (!sb || !read_sector)
        return 0;
    if (sb->journal_lba == 0 && sb->journal_sectors == 0)
        return 1;
    if (sb->journal_lba == 0 ||
        sb->journal_sectors < SFS2_JOURNAL_HEADER_SECTORS +
                              SFS2_JOURNAL_MAX_RECORDS)
        return 0;

    uint8_t header_sector[SFS2_SECTOR_SIZE];
    if (!read_sector(sb->journal_lba, header_sector))
        return 0;
    if (sfs2_sector_is_zero(header_sector))
        return 1;

    sfs2_journal_header_t header;
    const uint8_t *src = header_sector;
    uint8_t *dst = (uint8_t *)&header;
    for (uint32_t i = 0; i < sizeof(header); ++i)
        dst[i] = src[i];

    if (header.magic != SFS2_JOURNAL_MAGIC ||
        header.checksum != sfs2_checksum(&header, sizeof(header),
            (uint32_t)offsetof(sfs2_journal_header_t, checksum)))
        return 0;
    if (header.state == 0u)
        return header.record_count == 0u;
    if (header.state != SFS2_JOURNAL_COMMITTED ||
        header.record_count == 0u ||
        header.record_count > SFS2_JOURNAL_MAX_RECORDS ||
        !write_sector)
        return 0;

    uint8_t data[SFS2_SECTOR_SIZE];
    for (uint32_t i = 0; i < header.record_count; ++i) {
        uint64_t target = header.targets[i];
        uint64_t journal_end = sb->journal_lba + sb->journal_sectors;
        if (target >= sb->volume_sectors ||
            (target >= sb->journal_lba && target < journal_end))
            return 0;
        for (uint32_t j = 0; j < i; ++j)
            if (header.targets[j] == target)
                return 0;
        if (!read_sector(sb->journal_lba + 1u + i, data) ||
            header.data_checksums[i] !=
                sfs2_checksum(data, sizeof(data), SFS2_SECTOR_SIZE - 4u))
            return 0;
        if (!write_sector(target, data))
            return 0;
    }

    /* Clearing the commit header is the final durable journal operation. */
    for (uint32_t i = 0; i < SFS2_SECTOR_SIZE; ++i)
        header_sector[i] = 0;
    return write_sector(sb->journal_lba, header_sector);
}

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
        sb->root_inode != 1u)
        return 0;

    uint64_t bitmap_end = sb->bitmap_lba + sb->bitmap_sectors;
    uint64_t inode_end = sb->inode_table_lba + sb->inode_table_sectors;
    uint64_t journal_end = sb->journal_lba + sb->journal_sectors;
    int has_journal = sb->journal_lba != 0 || sb->journal_sectors != 0;
    if ((has_journal &&
         (sb->journal_lba < 1u ||
          sb->journal_sectors < SFS2_JOURNAL_HEADER_SECTORS +
                                SFS2_JOURNAL_MAX_RECORDS ||
          journal_end < sb->journal_lba ||
          journal_end > sb->volume_sectors ||
          sb->bitmap_lba < journal_end)) ||
        (!has_journal && sb->bitmap_lba < 1u) ||
        sb->bitmap_sectors == 0 ||
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
               inode->parent_id == 0u && inode->name[0] == '\0' &&
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


typedef struct {
    uint32_t count;
    uint64_t targets[SFS2_JOURNAL_MAX_RECORDS];
    uint8_t sectors[SFS2_JOURNAL_MAX_RECORDS][SFS2_SECTOR_SIZE];
} sfs2_transaction_t;

static sfs2_transaction_t g_sfs2_transaction;

static int sfs2_tx_stage(sfs2_volume_t *volume, uint64_t lba,
                         uint32_t *out_index) {
    sfs2_transaction_t *tx = &g_sfs2_transaction;
    if (!volume || !volume->mounted || !out_index ||
        lba >= volume->superblock.volume_sectors ||
        (lba >= volume->superblock.journal_lba &&
         lba < volume->superblock.journal_lba +
                   volume->superblock.journal_sectors))
        return 0;
    for (uint32_t i = 0; i < tx->count; ++i) {
        if (tx->targets[i] == lba) {
            *out_index = i;
            return 1;
        }
    }
    if (tx->count >= SFS2_JOURNAL_MAX_RECORDS ||
        !volume->read_sector(lba, tx->sectors[tx->count]))
        return 0;
    tx->targets[tx->count] = lba;
    *out_index = tx->count++;
    return 1;
}

static int sfs2_tx_commit(sfs2_volume_t *volume) {
    sfs2_transaction_t *tx = &g_sfs2_transaction;
    if (!volume || !volume->mounted || !volume->write_sector ||
        volume->superblock.journal_lba == 0 ||
        volume->superblock.journal_sectors <
            SFS2_JOURNAL_HEADER_SECTORS + SFS2_JOURNAL_MAX_RECORDS ||
        tx->count == 0 || tx->count > SFS2_JOURNAL_MAX_RECORDS)
        return 0;

    sfs2_journal_header_t header;
    sfs2_zero(&header, sizeof(header));
    header.magic = SFS2_JOURNAL_MAGIC;
    header.state = SFS2_JOURNAL_COMMITTED;
    header.record_count = tx->count;
    header.sequence = volume->superblock.generation + 1u;
    for (uint32_t i = 0; i < tx->count; ++i) {
        header.targets[i] = tx->targets[i];
        header.data_checksums[i] = sfs2_checksum(
            tx->sectors[i], SFS2_SECTOR_SIZE, SFS2_SECTOR_SIZE - 4u);
        if (!volume->write_sector(volume->superblock.journal_lba + 1u + i,
                                  tx->sectors[i])) {
            volume->mounted = 0;
            return 0;
        }
    }
    header.checksum = sfs2_checksum(&header, sizeof(header),
        (uint32_t)offsetof(sfs2_journal_header_t, checksum));
    if (!volume->write_sector(volume->superblock.journal_lba, &header)) {
        volume->mounted = 0;
        return 0;
    }

    /* Once committed, any interruption is repaired by replay on next mount. */
    for (uint32_t i = 0; i < tx->count; ++i) {
        if (!volume->write_sector(tx->targets[i], tx->sectors[i])) {
            volume->mounted = 0;
            return 0;
        }
    }
    uint8_t clean[SFS2_SECTOR_SIZE];
    sfs2_zero(clean, sizeof(clean));
    if (!volume->write_sector(volume->superblock.journal_lba, clean)) {
        volume->mounted = 0;
        return 0;
    }
    tx->count = 0;
    return 1;
}

static int sfs2_tx_bitmap_bit(sfs2_volume_t *volume, uint64_t bit,
                              int allocated) {
    uint64_t data_bits = volume->superblock.volume_sectors -
                         volume->superblock.data_lba;
    if (bit >= data_bits)
        return 0;
    uint64_t byte = bit >> 3;
    uint64_t bitmap_lba = volume->superblock.bitmap_lba +
                          byte / SFS2_SECTOR_SIZE;
    uint32_t index = 0;
    if (!sfs2_tx_stage(volume, bitmap_lba, &index))
        return 0;
    uint32_t offset = (uint32_t)(byte % SFS2_SECTOR_SIZE);
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    if (allocated)
        g_sfs2_transaction.sectors[index][offset] |= mask;
    else
        g_sfs2_transaction.sectors[index][offset] &= (uint8_t)~mask;
    return 1;
}

static int sfs2_find_free_run_disk(sfs2_volume_t *volume, uint32_t needed,
                                  uint64_t *out_start) {
    if (!volume || !out_start || needed == 0 ||
        needed > SFS2_MAX_FILE_SECTORS)
        return 0;
    uint64_t bits = volume->superblock.volume_sectors -
                    volume->superblock.data_lba;
    uint8_t sector[SFS2_SECTOR_SIZE];
    uint64_t cached = UINT64_MAX;
    uint32_t run = 0;
    for (uint64_t bit = 0; bit < bits; ++bit) {
        uint64_t byte = bit >> 3;
        uint64_t sector_index = byte / SFS2_SECTOR_SIZE;
        if (sector_index != cached) {
            if (sector_index >= volume->superblock.bitmap_sectors ||
                !volume->read_sector(volume->superblock.bitmap_lba +
                                     sector_index, sector))
                return 0;
            cached = sector_index;
        }
        uint8_t value = sector[byte % SFS2_SECTOR_SIZE];
        if ((value & (1u << (bit & 7u))) == 0) {
            if (++run == needed) {
                *out_start = bit + 1u - needed;
                return 1;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

static int sfs2_tx_extent(sfs2_volume_t *volume, uint64_t start,
                          uint32_t count, int allocated) {
    for (uint32_t i = 0; i < count; ++i)
        if (!sfs2_tx_bitmap_bit(volume, start + i, allocated))
            return 0;
    return 1;
}

static int sfs2_tx_store_inode(sfs2_volume_t *volume, uint32_t inode_index,
                               const sfs2_inode_t *inode) {
    uint64_t offset = (uint64_t)inode_index * sizeof(*inode);
    const uint8_t *src = (const uint8_t *)inode;
    for (uint32_t i = 0; i < sizeof(*inode); ++i) {
        uint64_t absolute = offset + i;
        uint64_t lba = volume->superblock.inode_table_lba +
                       absolute / SFS2_SECTOR_SIZE;
        uint32_t index = 0;
        if (!sfs2_tx_stage(volume, lba, &index))
            return 0;
        g_sfs2_transaction.sectors[index][absolute % SFS2_SECTOR_SIZE] =
            src[i];
    }
    return 1;
}

static int sfs2_tx_store_superblock(sfs2_volume_t *volume,
                                    sfs2_superblock_t *updated) {
    uint32_t index = 0;
    if (!sfs2_tx_stage(volume, 0u, &index))
        return 0;
    updated->generation++;
    updated->checksum = sfs2_checksum(updated, sizeof(*updated),
        (uint32_t)offsetof(sfs2_superblock_t, checksum));
    const uint8_t *src = (const uint8_t *)updated;
    for (uint32_t i = 0; i < sizeof(*updated); ++i)
        g_sfs2_transaction.sectors[index][i] = src[i];
    return 1;
}

static int sfs2_find_inode_id(const sfs2_volume_t *volume, uint32_t id,
                              uint32_t *out_index);
static int sfs2_next_inode_slot(sfs2_volume_t *volume, uint32_t *out_index,
                                uint32_t *out_id);

static int sfs2_find_child_entry(sfs2_volume_t *volume,
                                   uint32_t parent_id, const char *name,
                                   uint32_t length, uint32_t *out_index) {
    for (uint32_t i = 0; i < volume->superblock.inode_capacity; ++i) {
        sfs2_inode_t *inode = &volume->inodes[i];
        if (!inode->inode_id || inode->parent_id != parent_id)
            continue;
        uint32_t n = 0;
        while (n < SFS2_NAME_MAX && inode->name[n])
            ++n;
        if (n != length)
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

static int sfs2_create_directory_entry(sfs2_volume_t *volume,
                                       uint32_t parent_id, const char *name,
                                       uint32_t name_length,
                                       uint32_t *out_index) {
    uint32_t index = 0;
    uint32_t inode_id = 0;
    if (!sfs2_next_inode_slot(volume, &index, &inode_id))
        return 0;
    sfs2_inode_t directory;
    sfs2_zero(&directory, sizeof(directory));
    directory.inode_id = inode_id;
    directory.parent_id = parent_id;
    directory.type = SFS2_INODE_DIRECTORY;
    for (uint32_t i = 0; i < name_length; ++i)
        directory.name[i] = name[i];
    directory.generation = 1u;
    directory.checksum = sfs2_checksum(&directory, sizeof(directory),
        (uint32_t)offsetof(sfs2_inode_t, checksum));

    g_sfs2_transaction.count = 0;
    if (!sfs2_tx_store_inode(volume, index, &directory))
        return 0;
    sfs2_superblock_t updated_sb = volume->superblock;
    if (!sfs2_tx_store_superblock(volume, &updated_sb) ||
        !sfs2_tx_commit(volume))
        return 0;
    volume->superblock = updated_sb;
    volume->inodes[index] = directory;
    if (out_index)
        *out_index = index;
    return 1;
}

/* Resolve a file path to its parent directory and final component. Missing
   intermediate directories are created transactionally for write operations. */
static int sfs2_resolve_parent(sfs2_volume_t *volume, const char *path,
                               int create_parents, uint32_t *out_parent_id,
                               const char **out_name,
                               uint32_t *out_name_length) {
    if (!volume || !volume->mounted || !path || !out_parent_id ||
        !out_name || !out_name_length)
        return 0;
    uint32_t parent_index = 0;
    if (!sfs2_find_inode_id(volume, volume->superblock.root_inode,
                            &parent_index))
        return 0;
    uint32_t pos = 0;
    while (path[pos] == '/')
        ++pos;
    if (!path[pos])
        return 0;

    for (;;) {
        uint32_t start = pos;
        while (path[pos] && path[pos] != '/')
            ++pos;
        uint32_t length = pos - start;
        if (!length || length >= SFS2_NAME_MAX ||
            (length == 1u && path[start] == '.') ||
            (length == 2u && path[start] == '.' && path[start + 1u] == '.'))
            return 0;
        uint32_t next = pos;
        while (path[next] == '/')
            ++next;
        if (!path[next]) {
            if (next != pos)
                return 0;
            *out_parent_id = volume->inodes[parent_index].inode_id;
            *out_name = path + start;
            *out_name_length = length;
            return volume->inodes[parent_index].type ==
                   SFS2_INODE_DIRECTORY;
        }

        if (volume->inodes[parent_index].type != SFS2_INODE_DIRECTORY)
            return 0;
        uint32_t child_index = 0;
        if (!sfs2_find_child_entry(volume,
                volume->inodes[parent_index].inode_id,
                path + start, length, &child_index)) {
            if (!create_parents ||
                !sfs2_create_directory_entry(volume,
                    volume->inodes[parent_index].inode_id,
                    path + start, length, &child_index))
                return 0;
        }
        if (volume->inodes[child_index].type != SFS2_INODE_DIRECTORY)
            return 0;
        parent_index = child_index;
        pos = next;
    }
}

static int sfs2_next_inode_slot(sfs2_volume_t *volume, uint32_t *out_index,
                                uint32_t *out_id) {
    uint32_t free_index = UINT32_MAX;
    uint32_t max_id = 1u;
    for (uint32_t i = 0; i < volume->superblock.inode_capacity; ++i) {
        if (!volume->inodes[i].inode_id && free_index == UINT32_MAX)
            free_index = i;
        if (volume->inodes[i].inode_id > max_id)
            max_id = volume->inodes[i].inode_id;
    }
    if (free_index == UINT32_MAX || max_id == UINT32_MAX)
        return 0;
    *out_index = free_index;
    *out_id = max_id + 1u;
    return 1;
}

int sfs2_write_file(sfs2_volume_t *volume, const char *path,
                    const void *data, uint32_t length) {
    const char *name = 0;
    uint32_t name_length = 0;
    uint32_t parent_id = 0;
    if (!volume || !volume->mounted || !volume->write_sector ||
        volume->superblock.journal_lba == 0 ||
        (!data && length) ||
        !sfs2_resolve_parent(volume, path, 1, &parent_id,
                             &name, &name_length))
        return 0;
    uint64_t sectors64 = ((uint64_t)length + SFS2_SECTOR_SIZE - 1u) /
                         SFS2_SECTOR_SIZE;
    if (sectors64 > SFS2_MAX_FILE_SECTORS)
        return 0;
    uint32_t sectors = (uint32_t)sectors64;

    uint32_t index = 0;
    int exists = sfs2_find_child_entry(volume, parent_id, name, name_length, &index);
    if (exists && volume->inodes[index].type != SFS2_INODE_FILE)
        return 0;
    sfs2_inode_t previous;
    if (exists) {
        previous = volume->inodes[index];
        if (previous.data_sectors > SFS2_MAX_FILE_SECTORS)
            return 0;
    } else {
        uint32_t new_id = 0;
        if (!sfs2_next_inode_slot(volume, &index, &new_id))
            return 0;
        sfs2_zero(&previous, sizeof(previous));
        previous.inode_id = new_id;
        previous.parent_id = parent_id;
        previous.type = SFS2_INODE_FILE;
        for (uint32_t i = 0; i < name_length; ++i)
            previous.name[i] = name[i];
    }

    uint64_t new_start_bit = 0;
    if (sectors && !sfs2_find_free_run_disk(volume, sectors,
                                             &new_start_bit))
        return 0;
    uint64_t new_lba = sectors
        ? volume->superblock.data_lba + new_start_bit : 0;

    /* Write new data before committing metadata; old contents stay intact. */
    const uint8_t *source = (const uint8_t *)data;
    uint8_t sector[SFS2_SECTOR_SIZE];
    for (uint32_t i = 0; i < sectors; ++i) {
        sfs2_zero(sector, sizeof(sector));
        uint32_t offset = i * SFS2_SECTOR_SIZE;
        uint32_t remaining = length - offset;
        uint32_t chunk = remaining < SFS2_SECTOR_SIZE
            ? remaining : SFS2_SECTOR_SIZE;
        for (uint32_t j = 0; j < chunk; ++j)
            sector[j] = source[offset + j];
        if (!volume->write_sector(new_lba + i, sector))
            return 0;
    }

    sfs2_transaction_t *tx = &g_sfs2_transaction;
    tx->count = 0;
    if (exists && previous.data_sectors) {
        uint64_t old_bit = previous.data_lba -
                           volume->superblock.data_lba;
        if (!sfs2_tx_extent(volume, old_bit, previous.data_sectors, 0))
            return 0;
    }
    if (sectors && !sfs2_tx_extent(volume, new_start_bit, sectors, 1))
        return 0;

    sfs2_inode_t updated = previous;
    updated.size_bytes = length;
    updated.data_lba = new_lba;
    updated.data_sectors = sectors;
    updated.generation++;
    updated.checksum = sfs2_checksum(&updated, sizeof(updated),
        (uint32_t)offsetof(sfs2_inode_t, checksum));
    if (!sfs2_tx_store_inode(volume, index, &updated))
        return 0;
    sfs2_superblock_t updated_sb = volume->superblock;
    if (!sfs2_tx_store_superblock(volume, &updated_sb) ||
        !sfs2_tx_commit(volume))
        return 0;
    volume->superblock = updated_sb;
    volume->inodes[index] = updated;
    return 1;
}

int sfs2_unlink_file(sfs2_volume_t *volume, const char *path) {
    const char *name = 0;
    uint32_t name_length = 0;
    uint32_t parent_id = 0;
    if (!volume || !volume->mounted || !volume->write_sector ||
        volume->superblock.journal_lba == 0 ||
        !sfs2_resolve_parent(volume, path, 0, &parent_id,
                             &name, &name_length))
        return 0;
    uint32_t index = 0;
    if (!sfs2_find_child_entry(volume, parent_id, name, name_length, &index) ||
        volume->inodes[index].type != SFS2_INODE_FILE)
        return 0;
    sfs2_inode_t previous = volume->inodes[index];
    if (previous.data_sectors > SFS2_MAX_FILE_SECTORS)
        return 0;

    g_sfs2_transaction.count = 0;
    if (previous.data_sectors) {
        uint64_t old_bit = previous.data_lba -
                           volume->superblock.data_lba;
        if (!sfs2_tx_extent(volume, old_bit, previous.data_sectors, 0))
            return 0;
    }
    sfs2_inode_t empty;
    sfs2_zero(&empty, sizeof(empty));
    if (!sfs2_tx_store_inode(volume, index, &empty))
        return 0;
    sfs2_superblock_t updated_sb = volume->superblock;
    if (!sfs2_tx_store_superblock(volume, &updated_sb) ||
        !sfs2_tx_commit(volume))
        return 0;
    volume->superblock = updated_sb;
    volume->inodes[index] = empty;
    return 1;
}


static int sfs2_read_inode_table(sfs2_volume_t *volume) {
    uint8_t sector[SFS2_SECTOR_SIZE];
    uint8_t record[sizeof(sfs2_inode_t)];
    uint32_t record_bytes = 0;
    uint32_t inode_index = 0;
    uint64_t total_bytes =
        (uint64_t)volume->superblock.inode_capacity * sizeof(sfs2_inode_t);
    uint64_t loaded_bytes = 0;
    uint64_t cached_sector = UINT64_MAX;

    while (loaded_bytes < total_bytes) {
        uint64_t sector_index = loaded_bytes / SFS2_SECTOR_SIZE;
        if (sector_index >= volume->superblock.inode_table_sectors)
            return 0;
        if (sector_index != cached_sector) {
            if (!volume->read_sector(volume->superblock.inode_table_lba +
                                     sector_index, sector))
                return 0;
            cached_sector = sector_index;
        }
        uint32_t within = (uint32_t)(loaded_bytes % SFS2_SECTOR_SIZE);
        uint32_t chunk = SFS2_SECTOR_SIZE - within;
        uint64_t remaining = total_bytes - loaded_bytes;
        if (chunk > remaining)
            chunk = (uint32_t)remaining;
        for (uint32_t i = 0; i < chunk; ++i) {
            record[record_bytes++] = sector[within + i];
            if (record_bytes == sizeof(sfs2_inode_t)) {
                if (inode_index >= volume->superblock.inode_capacity)
                    return 0;
                uint8_t *dst = (uint8_t *)&volume->inodes[inode_index++];
                for (uint32_t j = 0; j < sizeof(sfs2_inode_t); ++j)
                    dst[j] = record[j];
                record_bytes = 0;
            }
        }
        loaded_bytes += chunk;
    }
    return inode_index == volume->superblock.inode_capacity &&
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

static int sfs2_validate_data_extents(sfs2_volume_t *volume) {
    uint8_t bitmap_sector[SFS2_SECTOR_SIZE];
    uint64_t cached_bitmap_sector = UINT64_MAX;
    for (uint32_t i = 0; i < volume->superblock.inode_capacity; ++i) {
        const sfs2_inode_t *inode = &volume->inodes[i];
        if (inode->inode_id == 0 || inode->type != SFS2_INODE_FILE ||
            inode->data_sectors == 0)
            continue;
        uint64_t end = inode->data_lba + inode->data_sectors;
        for (uint32_t j = 0; j < i; ++j) {
            const sfs2_inode_t *other = &volume->inodes[j];
            if (other->inode_id == 0 || other->type != SFS2_INODE_FILE ||
                other->data_sectors == 0)
                continue;
            uint64_t other_end = other->data_lba + other->data_sectors;
            if (inode->data_lba < other_end && other->data_lba < end)
                return 0;
        }
        for (uint64_t lba = inode->data_lba; lba < end; ++lba) {
            uint64_t bit = lba - volume->superblock.data_lba;
            uint64_t byte = bit >> 3;
            uint64_t bitmap_sector_index = byte / SFS2_SECTOR_SIZE;
            if (bitmap_sector_index >= volume->superblock.bitmap_sectors)
                return 0;
            if (bitmap_sector_index != cached_bitmap_sector) {
                if (!volume->read_sector(volume->superblock.bitmap_lba +
                                         bitmap_sector_index, bitmap_sector))
                    return 0;
                cached_bitmap_sector = bitmap_sector_index;
            }
            uint32_t byte_offset = (uint32_t)(byte % SFS2_SECTOR_SIZE);
            uint8_t mask = (uint8_t)(1u << (bit & 7u));
            if ((bitmap_sector[byte_offset] & mask) == 0)
                return 0;
        }
    }
    return 1;
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
    if (root_count != 1u)
        return 0;

    /* Every live inode must reach the single root without a parent cycle. */
    for (uint32_t i = 0; i < volume->superblock.inode_capacity; ++i) {
        uint32_t current = volume->inodes[i].inode_id;
        if (current == 0 || current == volume->superblock.root_inode)
            continue;
        uint32_t hops = 0;
        while (current != volume->superblock.root_inode) {
            uint32_t current_index = 0;
            if (++hops > volume->superblock.inode_capacity ||
                !sfs2_find_inode_id(volume, current, &current_index))
                return 0;
            current = volume->inodes[current_index].parent_id;
            if (current == 0)
                return 0;
        }
    }
    return 1;
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
    if (!sfs2_replay_journal(&volume->superblock, read_sector, write_sector))
        return 0;
    /* Replay may have committed a new superblock generation; reload it. */
    if (!read_sector(0, sector))
        return 0;
    src = sector;
    dst = (uint8_t *)&volume->superblock;
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
    if (!sfs2_validate_namespace(volume) ||
        !sfs2_validate_data_extents(volume)) {
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
static uint8_t g_sfs2_test_disk[8192u][SFS2_SECTOR_SIZE];
static uint8_t g_sfs2_large_payload[3u * 1024u * 1024u + 37u];
static uint64_t g_sfs2_test_fail_write_lba = UINT64_MAX;
static uint32_t g_sfs2_test_write_calls;
static uint32_t g_sfs2_test_fail_write_call;

static int sfs2_test_read(uint64_t lba, void *sector) {
    if (!sector || lba >= 8192u)
        return 0;
    const uint8_t *src = g_sfs2_test_disk[lba];
    uint8_t *dst = (uint8_t *)sector;
    for (uint32_t i = 0; i < SFS2_SECTOR_SIZE; ++i)
        dst[i] = src[i];
    return 1;
}

static int sfs2_test_write(uint64_t lba, const void *sector) {
    if (!sector || lba >= 64u || lba == g_sfs2_test_fail_write_lba)
        return 0;
    ++g_sfs2_test_write_calls;
    if (g_sfs2_test_fail_write_call &&
        g_sfs2_test_write_calls == g_sfs2_test_fail_write_call)
        return 0;
    const uint8_t *src = (const uint8_t *)sector;
    for (uint32_t i = 0; i < SFS2_SECTOR_SIZE; ++i)
        g_sfs2_test_disk[lba][i] = src[i];
    return 1;
}

static void sfs2_test_store_inode_at(uint32_t table_lba, uint32_t index,
                                     const sfs2_inode_t *inode) {
    uint32_t offset = index * (uint32_t)sizeof(*inode);
    const uint8_t *src = (const uint8_t *)inode;
    for (uint32_t i = 0; i < sizeof(*inode); ++i) {
        uint32_t absolute = offset + i;
        g_sfs2_test_disk[table_lba + absolute / SFS2_SECTOR_SIZE]
                        [absolute % SFS2_SECTOR_SIZE] = src[i];
    }
}

static void sfs2_test_store_inode(uint32_t index,
                                  const sfs2_inode_t *inode) {
    sfs2_test_store_inode_at(2u, index, inode);
}

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t state;
    uint32_t record_count;
    uint32_t checksum;
    uint64_t sequence;
    uint64_t targets[SFS2_JOURNAL_MAX_RECORDS];
    uint32_t data_checksums[SFS2_JOURNAL_MAX_RECORDS];
    uint8_t reserved[104];
} sfs2_test_journal_header_t;

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
    g_sfs2_test_disk[1][0] = 0x01u; /* data-area sector zero is allocated */
    g_sfs2_test_disk[3][0] = 'v'; g_sfs2_test_disk[3][1] = 'e';
    g_sfs2_test_disk[3][2] = 'x'; g_sfs2_test_disk[3][3] = 'o';
    g_sfs2_test_disk[3][4] = 's';

    static sfs2_volume_t volume;
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
    if (sfs2_list_directory(&volume, "/", &cursor, &listed) != 1 ||
        listed.type != SFS2_INODE_DIRECTORY || listed.name[0] != 'e')
        return 0;
    if (sfs2_stat_path(&volume, "/missing", &info))
        return 0;
    char tail[3];
    if (!sfs2_read_file_at(&volume, "/hello", 2u, tail, 3u, &bytes) ||
        bytes != 3u || tail[0] != 'x' || tail[1] != 'o' || tail[2] != 's')
        return 0;
    /* Old SFS2 images without a journal must remain read-only. */
    if (sfs2_write_file(&volume, "/hello", "X", 1u))
        return 0;
    /* A damaged inode checksum must prevent mounting. */
    g_sfs2_test_disk[2][sizeof(sfs2_inode_t) + 12u] ^= 0x80u;
    volume.mounted = 0;
    if (sfs2_mount(&volume, sfs2_test_read, NULL, 16u))
        return 0;
    /* A file extent whose bitmap bit is clear must also be rejected. */
    g_sfs2_test_disk[2][sizeof(sfs2_inode_t) + 12u] ^= 0x80u;
    g_sfs2_test_disk[1][0] = 0u;
    if (sfs2_mount(&volume, sfs2_test_read, NULL, 16u))
        return 0;

    /* A committed redo record must be replayed before metadata is mounted. */
    sfs2_zero(g_sfs2_test_disk, sizeof(g_sfs2_test_disk));
    sfs2_zero(&sb, sizeof(sb));
    sb.magic = SFS2_MAGIC;
    sb.version = SFS2_VERSION;
    sb.header_size = sizeof(sb);
    sb.sector_size = SFS2_SECTOR_SIZE;
    sb.volume_sectors = 64u;
    sb.generation = 2u;
    sb.journal_lba = 1u;
    sb.journal_sectors = 33u;
    sb.bitmap_lba = 34u;
    sb.bitmap_sectors = 1u;
    sb.inode_table_lba = 35u;
    sb.inode_table_sectors = 2u;
    sb.inode_capacity = 8u;
    sb.data_lba = 37u;
    sb.root_inode = 1u;
    sb.checksum = sfs2_checksum(&sb, sizeof(sb),
        (uint32_t)offsetof(sfs2_superblock_t, checksum));
    sb_src = (const uint8_t *)&sb;
    for (uint32_t i = 0; i < sizeof(sb); ++i)
        g_sfs2_test_disk[0][i] = sb_src[i];

    sfs2_zero(&inode, sizeof(inode));
    inode.inode_id = 1u;
    inode.type = SFS2_INODE_DIRECTORY;
    sfs2_test_finish_inode(&inode);
    sfs2_test_store_inode_at(35u, 0u, &inode);

    sfs2_zero(&inode, sizeof(inode));
    inode.inode_id = 2u;
    inode.parent_id = 1u;
    inode.type = SFS2_INODE_FILE;
    inode.name[0] = 'j'; inode.name[1] = 'o'; inode.name[2] = 'u';
    inode.name[3] = 'r'; inode.name[4] = 'n'; inode.name[5] = 'a';
    inode.name[6] = 'l';
    inode.size_bytes = 1u;
    inode.data_lba = 37u;
    inode.data_sectors = 1u;
    sfs2_test_finish_inode(&inode);
    sfs2_test_store_inode_at(35u, 1u, &inode);

    sfs2_zero(&inode, sizeof(inode));
    inode.inode_id = 3u;
    inode.parent_id = 1u;
    inode.type = SFS2_INODE_DIRECTORY;
    inode.name[0] = 'd'; inode.name[1] = 'i'; inode.name[2] = 'r';
    sfs2_test_finish_inode(&inode);
    sfs2_test_store_inode_at(35u, 2u, &inode);

    g_sfs2_test_disk[34][0] = 0x01u;
    g_sfs2_test_disk[37][0] = 'J';

    uint8_t *journal_data = g_sfs2_test_disk[2];
    journal_data[0] = 0xA5u;
    g_sfs2_test_disk[3][0] = 0x5Au;
    sfs2_test_journal_header_t journal;
    sfs2_zero(&journal, sizeof(journal));
    journal.magic = SFS2_JOURNAL_MAGIC;
    journal.state = SFS2_JOURNAL_COMMITTED;
    journal.record_count = 2u;
    journal.sequence = 7u;
    journal.targets[0] = 63u;
    journal.targets[1] = 62u;
    journal.data_checksums[0] = sfs2_checksum(
        journal_data, SFS2_SECTOR_SIZE, SFS2_SECTOR_SIZE - 4u);
    journal.data_checksums[1] = sfs2_checksum(
        g_sfs2_test_disk[3], SFS2_SECTOR_SIZE, SFS2_SECTOR_SIZE - 4u);
    journal.checksum = sfs2_checksum(&journal, sizeof(journal),
        (uint32_t)offsetof(sfs2_test_journal_header_t, checksum));
    const uint8_t *journal_src = (const uint8_t *)&journal;
    for (uint32_t i = 0; i < sizeof(journal); ++i)
        g_sfs2_test_disk[1][i] = journal_src[i];

    sfs2_zero(&volume, sizeof(volume));
    /* Simulate power loss after the first home-sector replay write. */
    g_sfs2_test_fail_write_lba = 62u;
    if (sfs2_mount(&volume, sfs2_test_read, sfs2_test_write, 64u))
        return 0;
    g_sfs2_test_fail_write_lba = UINT64_MAX;
    if (!sfs2_mount(&volume, sfs2_test_read, sfs2_test_write, 64u))
        return 0;
    if (g_sfs2_test_disk[63][0] != 0xA5u ||
        g_sfs2_test_disk[62][0] != 0x5Au ||
        !sfs2_sector_is_zero(g_sfs2_test_disk[1]))
        return 0;
    if (!sfs2_stat_path(&volume, "/journal", &info))
        return 0;

    /* Existing directory names must not be shadowed by regular files. */
    if (sfs2_write_file(&volume, "/dir", "D", 1u))
        return 0;

    /* Exercise journaled create, replace, readback, and unlink operations. */
    if (!sfs2_write_file(&volume, "/new", "N", 1u))
        return 0;
    char one = 0;
    uint32_t read_count = 0;
    if (!sfs2_read_file_at(&volume, "/new", 0u, &one, 1u, &read_count) ||
        read_count != 1u || one != 'N')
        return 0;
    if (!sfs2_write_file(&volume, "/journal", "R", 1u))
        return 0;
    one = 0;
    if (!sfs2_read_file_at(&volume, "/journal", 0u, &one, 1u, &read_count) ||
        read_count != 1u || one != 'R')
        return 0;
    if (!sfs2_unlink_file(&volume, "/new") ||
        sfs2_stat_path(&volume, "/new", &info))
        return 0;

    /* Nested parents are materialized as directory inodes automatically. */
    if (!sfs2_write_file(&volume, "/dir/sub/leaf.bin", "nested", 6u) ||
        !sfs2_stat_path(&volume, "/dir/sub", &info) ||
        info.type != SFS2_INODE_DIRECTORY)
        return 0;
    char nested[6];
    if (!sfs2_read_file_at(&volume, "/dir/sub/leaf.bin", 0u,
                           nested, sizeof(nested), &read_count) ||
        read_count != sizeof(nested) || nested[0] != 'n' ||
        nested[1] != 'e' || nested[2] != 's' || nested[3] != 't' ||
        nested[4] != 'e' || nested[5] != 'd')
        return 0;
    cursor = 0;
    if (sfs2_list_directory(&volume, "/dir", &cursor, &listed) != 1 ||
        listed.type != SFS2_INODE_DIRECTORY || listed.name[0] != 's')
        return 0;
    if (!sfs2_unlink_file(&volume, "/dir/sub/leaf.bin") ||
        sfs2_stat_path(&volume, "/dir/sub/leaf.bin", &info))
        return 0;

    /* Inject a failed write at every transaction boundary. Before the
       commit-header write the old file must survive; after it, replay must
       complete the replacement on the next mount. */
    static uint8_t sfs2_fault_baseline[64u][SFS2_SECTOR_SIZE];
    for (uint32_t l = 0; l < 64u; ++l)
        for (uint32_t b = 0; b < SFS2_SECTOR_SIZE; ++b)
            sfs2_fault_baseline[l][b] = g_sfs2_test_disk[l][b];
    for (uint32_t fail_at = 1u; fail_at <= 9u; ++fail_at) {
        for (uint32_t l = 0; l < 64u; ++l)
            for (uint32_t b = 0; b < SFS2_SECTOR_SIZE; ++b)
                g_sfs2_test_disk[l][b] = sfs2_fault_baseline[l][b];
        sfs2_zero(&volume, sizeof(volume));
        if (!sfs2_mount(&volume, sfs2_test_read, sfs2_test_write, 64u))
            return 0;
        g_sfs2_test_write_calls = 0;
        g_sfs2_test_fail_write_call = fail_at;
        if (sfs2_write_file(&volume, "/journal", "Z", 1u))
            return 0;
        g_sfs2_test_fail_write_call = 0;
        if (!volume.mounted &&
            !sfs2_mount(&volume, sfs2_test_read, sfs2_test_write, 64u))
            return 0;
        one = 0;
        if (!sfs2_read_file_at(&volume, "/journal", 0u, &one, 1u,
                               &read_count) || read_count != 1u ||
            one != (fail_at >= 6u ? 'Z' : 'R'))
            return 0;
    }
    if (!sfs2_sector_is_zero(g_sfs2_test_disk[1]))
        return 0;

    /* Exercise a kernel write larger than the former 2 MiB ceiling. */
    sfs2_zero(g_sfs2_test_disk, sizeof(g_sfs2_test_disk));
    sfs2_zero(&sb, sizeof(sb));
    sb.magic = SFS2_MAGIC;
    sb.version = SFS2_VERSION;
    sb.header_size = sizeof(sb);
    sb.sector_size = SFS2_SECTOR_SIZE;
    sb.volume_sectors = 8192u;
    sb.generation = 1u;
    sb.journal_lba = 1u;
    sb.journal_sectors = 33u;
    sb.bitmap_lba = 34u;
    sb.bitmap_sectors = 2u;
    sb.inode_table_lba = 36u;
    sb.inode_table_sectors = 2u;
    sb.inode_capacity = 8u;
    sb.data_lba = 38u;
    sb.root_inode = 1u;
    sb.checksum = sfs2_checksum(&sb, sizeof(sb),
        (uint32_t)offsetof(sfs2_superblock_t, checksum));
    sb_src = (const uint8_t *)&sb;
    for (uint32_t i = 0; i < sizeof(sb); ++i)
        g_sfs2_test_disk[0][i] = sb_src[i];
    sfs2_zero(&inode, sizeof(inode));
    inode.inode_id = 1u;
    inode.type = SFS2_INODE_DIRECTORY;
    sfs2_test_finish_inode(&inode);
    sfs2_test_store_inode_at(36u, 0u, &inode);
    for (uint32_t i = 0; i < sizeof(g_sfs2_large_payload); ++i)
        g_sfs2_large_payload[i] = (uint8_t)((i * 17u + 3u) & 0xFFu);
    sfs2_zero(&volume, sizeof(volume));
    if (!sfs2_mount(&volume, sfs2_test_read, sfs2_test_write, 8192u) ||
        !sfs2_write_file(&volume, "/large.bin", g_sfs2_large_payload,
                         (uint32_t)sizeof(g_sfs2_large_payload)))
        return 0;
    if (!sfs2_stat_path(&volume, "/large.bin", &info) ||
        info.size_bytes != sizeof(g_sfs2_large_payload))
        return 0;
    uint8_t sample = 0;
    if (!sfs2_read_file_at(&volume, "/large.bin", 0u, &sample, 1u,
                           &read_count) || read_count != 1u ||
        sample != g_sfs2_large_payload[0])
        return 0;
    uint64_t middle = sizeof(g_sfs2_large_payload) / 2u;
    if (!sfs2_read_file_at(&volume, "/large.bin", middle, &sample, 1u,
                           &read_count) || read_count != 1u ||
        sample != g_sfs2_large_payload[middle])
        return 0;
    uint64_t last = sizeof(g_sfs2_large_payload) - 1u;
    if (!sfs2_read_file_at(&volume, "/large.bin", last, &sample, 1u,
                           &read_count) || read_count != 1u ||
        sample != g_sfs2_large_payload[last])
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

    uint8_t bitmap[2] = {0x0Fu, 0x00u};
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
