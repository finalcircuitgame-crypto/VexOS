#ifndef VEXOS_SFS2_H
#define VEXOS_SFS2_H

#include <stdint.h>

#define SFS2_MAGIC 0x32534653u /* "SFS2" */
#define SFS2_VERSION 2u
#define SFS2_SECTOR_SIZE 512u
#define SFS2_NAME_MAX 48u
#define SFS2_INODE_MAX 256u
#define SFS2_INODE_FILE 1u
#define SFS2_INODE_DIRECTORY 2u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t sector_size;
    uint64_t volume_sectors;
    uint64_t generation;
    uint64_t bitmap_lba;
    uint32_t bitmap_sectors;
    uint64_t inode_table_lba;
    uint32_t inode_table_sectors;
    uint32_t inode_capacity;
    uint64_t data_lba;
    uint32_t root_inode;
    uint32_t checksum;
    uint8_t reserved[440];
} sfs2_superblock_t;

typedef struct __attribute__((packed)) {
    uint32_t inode_id;
    uint32_t parent_id;
    uint8_t type;
    uint8_t flags;
    uint16_t reserved0;
    char name[SFS2_NAME_MAX];
    uint64_t size_bytes;
    uint64_t data_lba;
    uint32_t data_sectors;
    uint32_t generation;
    uint32_t checksum;
    uint8_t reserved[8];
} sfs2_inode_t;


typedef int (*sfs2_read_sector_fn)(uint64_t lba, void *sector);
typedef int (*sfs2_write_sector_fn)(uint64_t lba, const void *sector);

typedef struct {
    sfs2_superblock_t superblock;
    sfs2_read_sector_fn read_sector;
    sfs2_write_sector_fn write_sector;
    uint8_t mounted;
    sfs2_inode_t inodes[SFS2_INODE_MAX];
} sfs2_volume_t;

typedef struct {
    uint32_t inode_id;
    uint32_t parent_id;
    uint8_t type;
    uint64_t size_bytes;
} sfs2_stat_t;

/* Read-only mount and namespace/file access. Mount never formats media. */
int sfs2_mount(sfs2_volume_t *volume, sfs2_read_sector_fn read_sector,
               sfs2_write_sector_fn write_sector, uint64_t device_sectors);
int sfs2_stat_path(sfs2_volume_t *volume, const char *path,
                   sfs2_stat_t *out_stat);
int sfs2_read_file_at(sfs2_volume_t *volume, const char *path,
                      uint64_t offset, void *out, uint32_t length,
                      uint32_t *out_read);
int sfs2_list_directory(sfs2_volume_t *volume, const char *path,
                        uint32_t *cursor, sfs2_inode_t *out_inode);

/* Pure metadata helpers. These do not perform disk I/O or format a volume. */
uint32_t sfs2_checksum(const void *data, uint32_t length,
                       uint32_t checksum_offset);
int sfs2_validate_superblock(const sfs2_superblock_t *sb,
                             uint64_t device_sectors);
int sfs2_validate_inode(const sfs2_inode_t *inode,
                        uint64_t data_lba, uint64_t volume_sectors);
int sfs2_bitmap_test(const uint8_t *bitmap, uint32_t bit_count,
                     uint32_t bit_index);
int sfs2_bitmap_find_free_run(const uint8_t *bitmap, uint32_t bit_count,
                              uint32_t run_length, uint32_t *out_start);

#ifdef VEXOS_CI_INPUT_TEST
int sfs2_ci_metadata_self_test(void);
int sfs2_ci_mount_read_test(void);
#endif

#endif
