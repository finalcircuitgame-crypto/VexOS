#include "simplefs.h"
#include "sfs2.h"
#include "ata_pio.h"
#include "ahci.h"
#include <stdint.h>

void serial_print(const char *str);

typedef enum {
  STORAGE_FS_NONE = 0,
  STORAGE_FS_SFS = 1,
  STORAGE_FS_FAT32 = 2,
  STORAGE_FS_SFS2 = 3,
} storage_fs_kind_t;

#define SFS_MAGIC 0x31465353u /* 'SFS1' */
#define SFS_VERSION 1u
#define SFS_SECTOR_SIZE 512u
#define SFS_MAX_FILES 64u
#define SFS_NAME_MAX 32u

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t table_lba;
  uint32_t table_sectors;
  uint32_t data_lba;
  uint32_t next_free_lba;
  uint32_t file_count;
  uint32_t rsvd;
} __attribute__((packed)) sfs_superblock_t;

typedef struct {
  uint8_t used;
  char name[SFS_NAME_MAX];
  uint32_t start_lba;
  uint32_t size_bytes;
  uint32_t rsvd;
} __attribute__((packed)) sfs_file_t;

static const ata_pio_device_t *g_dev;
static uint8_t g_storage_ready;
static uint8_t g_storage_uses_ahci;
static sfs_superblock_t g_sb;
static sfs_file_t g_table[SFS_MAX_FILES];

static storage_fs_kind_t g_kind = STORAGE_FS_NONE;
static uint8_t g_mounted;
static sfs2_volume_t g_sfs2;
static uint64_t storage_sector_limit(void);
static int storage_read_sector(uint32_t lba, void *buf);
static int storage_write_sector(uint32_t lba, const void *buf);

static int sfs2_storage_read(uint64_t lba, void *sector) {
  if (lba > 0xFFFFFFFFull || lba >= storage_sector_limit())
    return 0;
  return storage_read_sector((uint32_t)lba, sector);
}

static int sfs2_storage_write(uint64_t lba, const void *sector) {
  if (lba > 0xFFFFFFFFull || lba >= storage_sector_limit())
    return 0;
  return storage_write_sector((uint32_t)lba, sector);
}

// ---------------- FAT32 (minimal, root-dir, short names, read-only) ---------

#define FAT_SECTOR_SIZE 512u
#define FAT_MAX_FILES 128u

typedef struct {
  uint16_t bytes_per_sector;
  uint8_t sectors_per_cluster;
  uint16_t reserved_sector_count;
  uint8_t num_fats;
  uint32_t fat_size_sectors;
  uint32_t root_cluster;
  uint32_t fat_start_lba;
  uint32_t data_start_lba;
  uint32_t total_sectors;
  uint32_t cluster_count;
} fat32_info_t;

typedef struct {
  char name[32];
  uint32_t first_cluster;
  uint32_t size_bytes;
  uint8_t attr;
} fat32_file_t;

static fat32_info_t g_fat;
static fat32_file_t g_fat_files[FAT_MAX_FILES];
static uint32_t g_fat_file_count;

static int str_eq(const char *a, const char *b);

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static int storage_read_sector(uint32_t lba, void *buf) {
  if (!g_storage_ready || !buf)
    return 0;
  if (g_storage_uses_ahci)
    return ahci_read_sectors((uint64_t)lba, 1u, buf);
  return ata_pio_read28(g_dev, lba, 1u, buf);
}

static int storage_write_sector(uint32_t lba, const void *buf) {
  if (!g_storage_ready || !buf)
    return 0;
  if (g_storage_uses_ahci)
    return ahci_write_sectors((uint64_t)lba, 1u, buf);
  return ata_pio_write28(g_dev, lba, 1u, buf);
}

/* ATA PIO currently exposes only 28-bit LBA addressing. AHCI reports the
   actual device capacity, so use it whenever that backend is active. */
static uint64_t storage_sector_limit(void) {
  if (!g_storage_ready)
    return 0;
  if (g_storage_uses_ahci)
    return ahci_get_sector_count();
  return 0x10000000ull;
}

static int fat_read_sector(uint32_t lba, void *buf) {
  return storage_read_sector(lba, buf);
}

static uint32_t fat_cluster_to_lba(uint32_t cluster) {
  if (cluster < 2u || cluster >= g_fat.cluster_count + 2u)
    return 0;
  return g_fat.data_start_lba +
         (cluster - 2u) * (uint32_t)g_fat.sectors_per_cluster;
}

static int fat_get_fat_entry(uint32_t cluster, uint32_t *out_next) {
  if (!out_next || cluster < 2u ||
      cluster >= g_fat.cluster_count + 2u)
    return 0;
  uint32_t fat_off = cluster * 4u;
  uint32_t fat_sector_index = fat_off / FAT_SECTOR_SIZE;
  if (fat_sector_index >= g_fat.fat_size_sectors)
    return 0;
  uint32_t sec = g_fat.fat_start_lba + fat_sector_index;
  uint32_t off = fat_off % FAT_SECTOR_SIZE;
  uint8_t buf[FAT_SECTOR_SIZE];
  if (!fat_read_sector(sec, buf))
    return 0;
  *out_next = rd32(&buf[off]) & 0x0FFFFFFFu;
  return 1;
}

static void fat_trim_space(char *s) {
  if (!s)
    return;
  uint32_t n = 0;
  while (s[n])
    n++;
  while (n && (s[n - 1] == ' ')) {
    s[n - 1] = 0;
    n--;
  }
}

static void fat_build_short_name(char *out, uint32_t out_cap, const uint8_t *ent) {
  if (!out || out_cap == 0)
    return;
  out[0] = 0;

  char base[9];
  char ext[4];
  for (uint32_t i = 0; i < 8; i++)
    base[i] = (char)ent[i];
  base[8] = 0;
  for (uint32_t i = 0; i < 3; i++)
    ext[i] = (char)ent[8 + i];
  ext[3] = 0;
  fat_trim_space(base);
  fat_trim_space(ext);

  uint32_t w = 0;
  for (uint32_t i = 0; base[i] && (w + 1u) < out_cap; i++)
    out[w++] = base[i];
  if (ext[0] && (w + 1u) < out_cap) {
    out[w++] = '.';
    for (uint32_t i = 0; ext[i] && (w + 1u) < out_cap; i++)
      out[w++] = ext[i];
  }
  out[w] = 0;
}

static int fat_is_fat32_boot_sector(const uint8_t *b) {
  if (!b)
    return 0;
  if (b[510] != 0x55 || b[511] != 0xAA)
    return 0;
  uint16_t bps = rd16(&b[11]);
  if (bps != 512)
    return 0;
  uint16_t root_ent = rd16(&b[17]);
  uint16_t fatsz16 = rd16(&b[22]);
  uint32_t fatsz32 = rd32(&b[36]);
  if (root_ent != 0)
    return 0;
  if (fatsz16 != 0)
    return 0;
  if (fatsz32 == 0)
    return 0;
  // Optional check: "FAT32   " at offset 82.
  if (b[82] != 'F' || b[83] != 'A' || b[84] != 'T' || b[85] != '3' || b[86] != '2')
    return 0;
  return 1;
}

static int fat_mount_and_scan_root(void) {
  uint8_t b[FAT_SECTOR_SIZE];
  if (!fat_read_sector(0, b))
    return 0;

  if (!fat_is_fat32_boot_sector(b))
    return 0;

  g_fat.bytes_per_sector = rd16(&b[11]);
  g_fat.sectors_per_cluster = b[13];
  g_fat.reserved_sector_count = rd16(&b[14]);
  g_fat.num_fats = b[16];
  g_fat.total_sectors = rd32(&b[32]);
  g_fat.fat_size_sectors = rd32(&b[36]);
  g_fat.root_cluster = rd32(&b[44]);
  if (g_fat.bytes_per_sector != FAT_SECTOR_SIZE ||
      g_fat.sectors_per_cluster == 0 ||
      g_fat.num_fats == 0 || g_fat.fat_size_sectors == 0 ||
      g_fat.total_sectors == 0 || g_fat.root_cluster < 2u) {
    return 0;
  }

  g_fat.fat_start_lba = (uint32_t)g_fat.reserved_sector_count;
  uint64_t data_start =
      (uint64_t)g_fat.fat_start_lba +
      (uint64_t)g_fat.num_fats * (uint64_t)g_fat.fat_size_sectors;
  if (data_start > 0xFFFFFFFFull || data_start >= g_fat.total_sectors)
    return 0;
  g_fat.data_start_lba = (uint32_t)data_start;
  g_fat.cluster_count =
      (g_fat.total_sectors - g_fat.data_start_lba) /
      (uint32_t)g_fat.sectors_per_cluster;
  if (g_fat.cluster_count == 0 ||
      g_fat.cluster_count > 0x0FFFFFF5u ||
      g_fat.root_cluster >= g_fat.cluster_count + 2u)
    return 0;

  g_fat_file_count = 0;
  for (uint32_t i = 0; i < FAT_MAX_FILES; i++) {
    g_fat_files[i].name[0] = 0;
    g_fat_files[i].first_cluster = 0;
    g_fat_files[i].size_bytes = 0;
    g_fat_files[i].attr = 0;
  }

  uint32_t cluster = g_fat.root_cluster;
  uint32_t budget_clusters = 256;
  while (budget_clusters--) {
    uint32_t lba0 = fat_cluster_to_lba(cluster);
    for (uint32_t s = 0; s < (uint32_t)g_fat.sectors_per_cluster; s++) {
      uint8_t sec[FAT_SECTOR_SIZE];
      if (!fat_read_sector(lba0 + s, sec))
        return 0;
      for (uint32_t off = 0; off + 32u <= FAT_SECTOR_SIZE; off += 32u) {
        const uint8_t *ent = &sec[off];
        uint8_t first = ent[0];
        if (first == 0x00)
          return 1;
        if (first == 0xE5)
          continue;
        uint8_t attr = ent[11];
        if (attr == 0x0F)
          continue; // LFN
        if (attr & 0x08)
          continue; // volume label
        if (attr & 0x10)
          continue; // directories (for now)

        if (g_fat_file_count >= FAT_MAX_FILES)
          continue;

        char name[32];
        fat_build_short_name(name, (uint32_t)sizeof(name), ent);
        if (!name[0])
          continue;

        uint32_t hi = (uint32_t)rd16(&ent[20]);
        uint32_t lo = (uint32_t)rd16(&ent[26]);
        uint32_t fc = (hi << 16) | lo;
        uint32_t sz = rd32(&ent[28]);

        fat32_file_t *f = &g_fat_files[g_fat_file_count++];
        for (uint32_t i = 0; i < (uint32_t)sizeof(f->name); i++)
          f->name[i] = 0;
        for (uint32_t i = 0; i + 1u < (uint32_t)sizeof(f->name) && name[i]; i++) {
          f->name[i] = name[i];
          f->name[i + 1u] = 0;
        }
        f->first_cluster = fc;
        f->size_bytes = sz;
        f->attr = attr;
      }
    }
    uint32_t next = 0;
    if (!fat_get_fat_entry(cluster, &next))
      return 0;
    if (next >= 0x0FFFFFF8u)
      return 1;
    if (next < 2u || next >= g_fat.cluster_count + 2u)
      return 0;
    cluster = next;
  }
  return 1;
}

static int fat_find(const char *name, uint32_t *out_idx) {
  if (!name)
    return 0;
  for (uint32_t i = 0; i < g_fat_file_count; i++) {
    if (str_eq(g_fat_files[i].name, name)) {
      if (out_idx)
        *out_idx = i;
      return 1;
    }
  }
  return 0;
}

static int sfs_find(const char *name, uint32_t *out_idx);

static void mem_zero(void *p, uint32_t n) {
  uint8_t *b = (uint8_t *)p;
  for (uint32_t i = 0; i < n; i++)
    b[i] = 0;
}

static uint32_t str_len32(const char *s) {
  uint32_t n = 0;
  if (!s)
    return 0;
  while (s[n] && n < 0x7FFFFFFFu)
    n++;
  return n;
}

static int str_eq(const char *a, const char *b) {
  if (!a || !b)
    return 0;
  for (;;) {
    char ca = *a++;
    char cb = *b++;
    if (ca != cb)
      return 0;
    if (!ca)
      return 1;
  }
}

static void str_copy(char *dst, const char *src, uint32_t maxn) {
  if (!dst || maxn == 0)
    return;
  uint32_t i = 0;
  if (src) {
    for (; i + 1 < maxn && src[i]; i++) {
      dst[i] = src[i];
    }
  }
  dst[i] = 0;
}

static int sfs_read_sector(uint32_t lba, void *buf) {
  return storage_read_sector(lba, buf);
}
static int sfs_write_sector(uint32_t lba, const void *buf) {
  return storage_write_sector(lba, buf);
}

static int sfs_load_table(void) {
  uint8_t sector[SFS_SECTOR_SIZE];
  mem_zero(&g_table[0], (uint32_t)sizeof(g_table));
  uint32_t idx = 0;
  for (uint32_t s = 0; s < g_sb.table_sectors; s++) {
    if (!sfs_read_sector(g_sb.table_lba + s, sector))
      return 0;
    for (uint32_t off = 0; off + sizeof(sfs_file_t) <= SFS_SECTOR_SIZE; off += sizeof(sfs_file_t)) {
      if (idx >= SFS_MAX_FILES)
        return 1;
      // Copy entry
      const uint8_t *src = &sector[off];
      uint8_t *dst = (uint8_t *)&g_table[idx];
      for (uint32_t i = 0; i < (uint32_t)sizeof(sfs_file_t); i++)
        dst[i] = src[i];
      idx++;
    }
  }
  return 1;
}

/* Validate persisted names and extents before exposing any table entry to
   VFS. SFS1 stores complete relative paths in a fixed 32-byte name field. */
static int sfs_valid_stored_name(const char *name, uint32_t *out_len) {
  uint32_t len = 0;
  while (len < SFS_NAME_MAX && name[len])
    ++len;
  if (len == 0 || len >= SFS_NAME_MAX || len > 31u)
    return 0;

  uint32_t component_len = 0;
  for (uint32_t i = 0; i <= len; ++i) {
    if (i == len || name[i] == '/') {
      if (component_len == 0 ||
          (component_len == 1u && name[i - 1u] == '.') ||
          (component_len == 2u && name[i - 1u] == '.' &&
           name[i - 2u] == '.'))
        return 0;
      component_len = 0;
    } else {
      ++component_len;
    }
  }
  if (out_len)
    *out_len = len;
  return 1;
}

static int sfs_names_conflict(const char *a, uint32_t a_len,
                              const char *b, uint32_t b_len) {
  uint32_t common = a_len < b_len ? a_len : b_len;
  for (uint32_t i = 0; i < common; ++i)
    if (a[i] != b[i])
      return 0;
  if (a_len == b_len)
    return 1;
  if (a_len < b_len)
    return b[a_len] == '/';
  return a[b_len] == '/';
}

static int sfs_validate_table(void) {
  uint32_t actual_count = 0;
  uint64_t device_limit = storage_sector_limit();
  if (device_limit == 0 || g_sb.data_lba >= device_limit ||
      g_sb.next_free_lba > device_limit)
    return 0;

  for (uint32_t i = 0; i < SFS_MAX_FILES; ++i) {
    const sfs_file_t *entry = &g_table[i];
    if (entry->used > 1u)
      return 0;
    if (!entry->used)
      continue;

    ++actual_count;
    uint32_t name_len = 0;
    if (!sfs_valid_stored_name(entry->name, &name_len))
      return 0;

    uint64_t sectors = ((uint64_t)entry->size_bytes +
                        (SFS_SECTOR_SIZE - 1u)) / SFS_SECTOR_SIZE;
    if (sectors == 0)
      sectors = 1;
    uint64_t end_lba = (uint64_t)entry->start_lba + sectors;
    if (entry->start_lba < g_sb.data_lba ||
        end_lba > g_sb.next_free_lba || end_lba > device_limit)
      return 0;

    for (uint32_t j = 0; j < i; ++j) {
      const sfs_file_t *previous = &g_table[j];
      if (!previous->used)
        continue;
      uint32_t previous_len = 0;
      if (!sfs_valid_stored_name(previous->name, &previous_len) ||
          sfs_names_conflict(entry->name, name_len,
                             previous->name, previous_len))
        return 0;

      uint64_t previous_sectors =
          ((uint64_t)previous->size_bytes +
           (SFS_SECTOR_SIZE - 1u)) / SFS_SECTOR_SIZE;
      if (previous_sectors == 0)
        previous_sectors = 1;
      uint64_t previous_end =
          (uint64_t)previous->start_lba + previous_sectors;
      if ((uint64_t)entry->start_lba < previous_end &&
          (uint64_t)previous->start_lba < end_lba)
        return 0;
    }
  }
  return actual_count == g_sb.file_count;
}

#ifdef VEXOS_CI_INPUT_TEST
static void sfs_ci_set_entry(uint32_t index, const char *name,
                             uint32_t start_lba, uint32_t size_bytes) {
  mem_zero(&g_table[index], (uint32_t)sizeof(g_table[index]));
  g_table[index].used = 1;
  str_copy(g_table[index].name, name, SFS_NAME_MAX);
  g_table[index].start_lba = start_lba;
  g_table[index].size_bytes = size_bytes;
}

/* Exercise the metadata validator with synthetic in-memory tables. No disk
   sectors are modified by this test. */
int simplefs_ci_validate_metadata_self_test(void) {
  sfs_file_t saved_table[SFS_MAX_FILES];
  for (uint32_t i = 0; i < SFS_MAX_FILES; ++i)
    saved_table[i] = g_table[i];
  sfs_superblock_t saved_superblock = g_sb;
  int ok = 1;

  mem_zero(g_table, (uint32_t)sizeof(g_table));
  g_sb.data_lba = 9u;
  g_sb.next_free_lba = 16u;
  g_sb.file_count = 1u;
  sfs_ci_set_entry(0u, "docs/file.txt", 9u, 512u);
  if (!sfs_validate_table())
    ok = 0;

  if (ok) {
    str_copy(g_table[0].name, "../escape", SFS_NAME_MAX);
    if (sfs_validate_table())
      ok = 0;
  }

  if (ok) {
    for (uint32_t i = 0; i < SFS_NAME_MAX; ++i)
      g_table[0].name[i] = 'x';
    if (sfs_validate_table())
      ok = 0;
  }

  if (ok) {
    mem_zero(g_table, (uint32_t)sizeof(g_table));
    g_sb.file_count = 2u;
    sfs_ci_set_entry(0u, "node", 9u, 1u);
    sfs_ci_set_entry(1u, "node/child", 10u, 1u);
    if (sfs_validate_table())
      ok = 0;
  }

  if (ok) {
    mem_zero(g_table, (uint32_t)sizeof(g_table));
    g_sb.file_count = 2u;
    sfs_ci_set_entry(0u, "first", 9u, 1024u);
    sfs_ci_set_entry(1u, "second", 10u, 1u);
    if (sfs_validate_table())
      ok = 0;
  }

  if (ok) {
    mem_zero(g_table, (uint32_t)sizeof(g_table));
    g_sb.file_count = 1u;
    g_sb.next_free_lba = 10u;
    sfs_ci_set_entry(0u, "past-end", 10u, 1024u);
    if (sfs_validate_table())
      ok = 0;
  }

  for (uint32_t i = 0; i < SFS_MAX_FILES; ++i)
    g_table[i] = saved_table[i];
  g_sb = saved_superblock;
  return ok;
}
#endif

int simplefs_file_size(const char *name, uint32_t *out_size) {
  if (!g_storage_ready || !name)
    return 0;
  if (g_kind == STORAGE_FS_SFS2) {
    sfs2_stat_t info;
    if (!sfs2_stat_path(&g_sfs2, name, &info) ||
        info.type != SFS2_INODE_FILE || info.size_bytes > UINT32_MAX)
      return 0;
    if (out_size)
      *out_size = (uint32_t)info.size_bytes;
    return 1;
  }
  if (g_kind == STORAGE_FS_FAT32) {
    uint32_t idx = 0;
    if (!fat_find(name, &idx))
      return 0;
    if (out_size)
      *out_size = g_fat_files[idx].size_bytes;
    return 1;
  }
  if (g_kind == STORAGE_FS_SFS) {
    uint32_t idx = 0;
    if (!sfs_find(name, &idx))
      return 0;
    if (out_size)
      *out_size = g_table[idx].size_bytes;
    return 1;
  }
  return 0;
}

static int sfs_store_table(void) {
  uint8_t sector[SFS_SECTOR_SIZE];
  uint32_t idx = 0;
  for (uint32_t s = 0; s < g_sb.table_sectors; s++) {
    mem_zero(sector, SFS_SECTOR_SIZE);
    for (uint32_t off = 0; off + sizeof(sfs_file_t) <= SFS_SECTOR_SIZE; off += sizeof(sfs_file_t)) {
      if (idx >= SFS_MAX_FILES)
        break;
      const uint8_t *src = (const uint8_t *)&g_table[idx];
      for (uint32_t i = 0; i < (uint32_t)sizeof(sfs_file_t); i++)
        sector[off + i] = src[i];
      idx++;
    }
    if (!sfs_write_sector(g_sb.table_lba + s, sector))
      return 0;
  }
  return 1;
}

static int sfs_store_superblock(void) {
  uint8_t sector[SFS_SECTOR_SIZE];
  mem_zero(sector, SFS_SECTOR_SIZE);
  const uint8_t *src = (const uint8_t *)&g_sb;
  for (uint32_t i = 0; i < (uint32_t)sizeof(g_sb); i++)
    sector[i] = src[i];
  return sfs_write_sector(0, sector);
}

static int sfs_load_superblock(void) {
  uint8_t sector[SFS_SECTOR_SIZE];
  if (!sfs_read_sector(0, sector))
    return 0;
  uint8_t *dst = (uint8_t *)&g_sb;
  for (uint32_t i = 0; i < (uint32_t)sizeof(g_sb); i++)
    dst[i] = sector[i];
  return 1;
}

int simplefs_init(void) {
  /* Mounting is a one-time operation. Re-running ATA IDENTIFY after the
     AHCI controller and runtime interrupts are active can stall the desktop;
     callers may safely request initialization more than once. */
  /*
   * Mounting is deliberately non-destructive. A future installer may add a
   * separate format operation after explicit confirmation; normal boot and
   * VFS initialization must never format unknown or damaged media.
   */
  if (g_mounted)
    return 1;
  g_kind = STORAGE_FS_NONE;

  /*
   * Prefer legacy ATA PIO when present (the pc machine uses it for the
   * SimpleFS disk). Q35 exposes SATA through AHCI instead, so fall back to
   * the AHCI driver initialized during PCI enumeration.
   */
  g_dev = NULL;
  g_storage_ready = 0;
  g_storage_uses_ahci = 0;

  if (ata_pio_init()) {
    g_dev = ata_pio_get_storage_device();
    g_storage_ready = (g_dev != NULL);
  }

  if (!g_storage_ready && ahci_is_ready()) {
    g_storage_uses_ahci = 1;
    g_storage_ready = 1;
    serial_print("[SFS] using AHCI storage backend\n");
  }

  if (!g_storage_ready) {
    serial_print("[SFS] no disk\n");
    return 0;
  }

  /* Probe SFS2 before legacy formats; journal geometry controls write support. */
  if (sfs2_mount(&g_sfs2, sfs2_storage_read, sfs2_storage_write,
                 storage_sector_limit())) {
    g_kind = STORAGE_FS_SFS2;
    if (g_sfs2.superblock.journal_lba)
      serial_print("[SFS2] mounted with redo journal\n");
    else
      serial_print("[SFS2] mounted read-only (no journal)\n");
    g_mounted = 1;
    return 1;
  }

  // Prefer FAT32 if detected on the storage disk.
  if (fat_mount_and_scan_root()) {
    g_kind = STORAGE_FS_FAT32;
    serial_print("[FAT32] mounted\n");
    g_mounted = 1;
    return 1;
  }
  g_kind = STORAGE_FS_SFS;

  if (!sfs_load_superblock()) {
    serial_print("[SFS] read superblock failed\n");
    g_kind = STORAGE_FS_NONE;
    return 0;
  }

  uint64_t table_end =
      (uint64_t)g_sb.table_lba + (uint64_t)g_sb.table_sectors;
  if (g_sb.magic != SFS_MAGIC || g_sb.version != SFS_VERSION ||
      g_sb.table_lba == 0 || g_sb.table_sectors != 8u ||
      table_end > storage_sector_limit() ||
      g_sb.data_lba < table_end ||
      g_sb.next_free_lba < g_sb.data_lba ||
      g_sb.next_free_lba > storage_sector_limit() ||
      g_sb.file_count > SFS_MAX_FILES) {
    serial_print("[SFS] unrecognized or invalid SFS1 volume; refusing to format\n");
    g_kind = STORAGE_FS_NONE;
    return 0;
  }

  if (!sfs_load_table()) {
    serial_print("[SFS] file table unreadable; refusing destructive recovery\n");
    g_kind = STORAGE_FS_NONE;
    return 0;
  }

  serial_print("[SFS] mounted\n");
  g_mounted = 1;
  return 1;
}

void simplefs_list(void) {
  if (g_kind == STORAGE_FS_FAT32) {
    serial_print("[FAT32] files:\n");
    for (uint32_t i = 0; i < g_fat_file_count; i++) {
      serial_print(" - ");
      serial_print(g_fat_files[i].name);
      serial_print("\n");
    }
    return;
  }

  serial_print("[SFS] files:\n");
  for (uint32_t i = 0; i < SFS_MAX_FILES; i++) {
    if (!g_table[i].used)
      continue;
    serial_print(" - ");
    serial_print(g_table[i].name);
    serial_print("\n");
  }
}

static int sfs_find(const char *name, uint32_t *out_idx) {
  for (uint32_t i = 0; i < SFS_MAX_FILES; i++) {
    if (g_table[i].used && str_eq(g_table[i].name, name)) {
      if (out_idx)
        *out_idx = i;
      return 1;
    }
  }
  return 0;
}

static int sfs_alloc_entry(uint32_t *out_idx) {
  for (uint32_t i = 0; i < SFS_MAX_FILES; i++) {
    if (!g_table[i].used) {
      if (out_idx)
        *out_idx = i;
      return 1;
    }
  }
  return 0;
}


static int sfs2_build_relative_path(uint32_t inode_index, char *out,
                                    uint32_t out_cap) {
  uint32_t chain[SFS2_INODE_MAX];
  uint32_t depth = 0;
  uint32_t current = inode_index;
  while (depth < SFS2_INODE_MAX) {
    const sfs2_inode_t *inode = &g_sfs2.inodes[current];
    if (inode->inode_id == g_sfs2.superblock.root_inode)
      break;
    chain[depth++] = current;
    uint32_t parent_found = SFS2_INODE_MAX;
    for (uint32_t i = 0; i < g_sfs2.superblock.inode_capacity; ++i) {
      if (g_sfs2.inodes[i].inode_id == inode->parent_id) {
        parent_found = i;
        break;
      }
    }
    if (parent_found == SFS2_INODE_MAX)
      return 0;
    current = parent_found;
  }
  if (depth == 0 || depth >= SFS2_INODE_MAX)
    return 0;
  uint32_t written = 0;
  while (depth) {
    const char *name = g_sfs2.inodes[chain[--depth]].name;
    uint32_t n = str_len32(name);
    if (!n || written + n + (written ? 1u : 0u) >= out_cap)
      return 0;
    if (written)
      out[written++] = '/';
    for (uint32_t j = 0; j < n; ++j)
      out[written++] = name[j];
  }
  out[written] = 0;
  return 1;
}

uint32_t simplefs_file_count(void) {
  if (g_kind == STORAGE_FS_SFS2) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < g_sfs2.superblock.inode_capacity; ++i)
      if (g_sfs2.inodes[i].inode_id &&
          g_sfs2.inodes[i].inode_id != g_sfs2.superblock.root_inode)
        ++count;
    return count;
  }
  if (g_kind == STORAGE_FS_FAT32)
    return g_fat_file_count;
  if (g_kind == STORAGE_FS_SFS) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < SFS_MAX_FILES; i++) {
      if (g_table[i].used)
        n++;
    }
    return n;
  }
  return 0;
}

int simplefs_file_name_at(uint32_t index, char *out_name, uint32_t out_cap) {
  if (!out_name || out_cap == 0)
    return 0;

  if (g_kind == STORAGE_FS_SFS2) {
    uint32_t current = 0;
    for (uint32_t i = 0; i < g_sfs2.superblock.inode_capacity; ++i) {
      if (!g_sfs2.inodes[i].inode_id ||
          g_sfs2.inodes[i].inode_id == g_sfs2.superblock.root_inode)
        continue;
      if (current++ == index)
        return sfs2_build_relative_path(i, out_name, out_cap);
    }
    out_name[0] = 0;
    return 0;
  }

  if (g_kind == STORAGE_FS_FAT32) {
    if (index >= g_fat_file_count) {
      out_name[0] = 0;
      return 0;
    }
    str_copy(out_name, g_fat_files[index].name, out_cap);
    return 1;
  }

  uint32_t n = 0;
  for (uint32_t i = 0; i < SFS_MAX_FILES; i++) {
    if (!g_table[i].used)
      continue;
    if (n == index) {
      str_copy(out_name, g_table[i].name, out_cap);
      return 1;
    }
    n++;
  }
  out_name[0] = 0;
  return 0;
}

uint32_t simplefs_list_to_buffer(char *out, uint32_t out_cap) {
  if (!out || out_cap == 0)
    return 0;

  if (g_kind == STORAGE_FS_SFS2) {
    uint32_t written = 0;
    for (uint32_t i = 0; i < simplefs_file_count(); ++i) {
      char path[128u];
      if (!simplefs_file_name_at(i, path, sizeof(path)))
        continue;
      uint32_t n = str_len32(path);
      if (written + n + 1u >= out_cap)
        break;
      for (uint32_t j = 0; j < n; ++j)
        out[written++] = path[j];
      out[written++] = '\n';
    }
    out[written] = 0;
    return written;
  }

  if (g_kind == STORAGE_FS_FAT32) {
    uint32_t w = 0;
    for (uint32_t i = 0; i < g_fat_file_count; i++) {
      const char *s = g_fat_files[i].name;
      for (uint32_t j = 0; s[j] && (w + 1u) < out_cap; j++)
        out[w++] = s[j];
      if ((w + 1u) < out_cap)
        out[w++] = '\n';
    }
    out[w] = 0;
    return w;
  }

  uint32_t w = 0;
  for (uint32_t i = 0; i < SFS_MAX_FILES; i++) {
    if (!g_table[i].used)
      continue;

    const char *s = g_table[i].name;
    for (uint32_t j = 0; s[j] && (w + 1u) < out_cap; j++) {
      out[w++] = s[j];
    }
    if ((w + 1u) < out_cap)
      out[w++] = '\n';
  }
  out[w] = 0;
  return w;
}

int simplefs_delete_file(const char *name) {
  if (!g_storage_ready || !name || str_len32(name) == 0)
    return 0;
  if (g_kind == STORAGE_FS_SFS2)
    return sfs2_unlink_file(&g_sfs2, name);
  if (g_kind != STORAGE_FS_SFS || str_len32(name) >= SFS_NAME_MAX)
    return 0;

  uint32_t idx = 0;
  if (!sfs_find(name, &idx))
    return 0;

  sfs_file_t previous = g_table[idx];
  mem_zero(&g_table[idx], (uint32_t)sizeof(g_table[idx]));

  uint32_t count = 0;
  for (uint32_t i = 0; i < SFS_MAX_FILES; ++i)
    if (g_table[i].used)
      ++count;
  g_sb.file_count = count;

  if (!sfs_store_table()) {
    g_table[idx] = previous;
    g_sb.file_count = count + 1u;
    (void)sfs_store_table();
    (void)sfs_store_superblock();
    return 0;
  }
  if (!sfs_store_superblock()) {
    g_table[idx] = previous;
    g_sb.file_count = count + 1u;
    (void)sfs_store_table();
    (void)sfs_store_superblock();
    return 0;
  }

  /* SFS1 is append-only: the directory entry is reclaimed, but its old
     data sectors remain unavailable until a future versioned free-space map. */
  return 1;
}

int simplefs_write_file(const char *name, const void *data, uint32_t len) {
  /* Never let direct SimpleFS callers write after a failed/unmounted probe. */
  if (!g_storage_ready || !name || (!data && len) || str_len32(name) == 0)
    return 0;
  if (g_kind == STORAGE_FS_SFS2)
    return sfs2_write_file(&g_sfs2, name, data, len);
  /* SFS1 writes are allowed only after a successful SFS1 mount. */
  if (g_kind != STORAGE_FS_SFS || !data ||
      str_len32(name) >= SFS_NAME_MAX)
    return 0;

  uint32_t idx = 0;
  if (!sfs_find(name, &idx)) {
    if (!sfs_alloc_entry(&idx))
      return 0;
  }

  uint64_t sectors64 =
      ((uint64_t)len + (SFS_SECTOR_SIZE - 1u)) / SFS_SECTOR_SIZE;
  if (sectors64 == 0)
    sectors64 = 1;
  uint64_t end_lba = (uint64_t)g_sb.next_free_lba + sectors64;
  uint64_t device_limit = storage_sector_limit();
  if (sectors64 > UINT32_MAX || end_lba > device_limit ||
      end_lba > 0xFFFFFFFFull)
    return 0;
  uint32_t sectors = (uint32_t)sectors64;
  uint32_t start_lba = g_sb.next_free_lba;

  const uint8_t *src = (const uint8_t *)data;
  uint8_t sector[SFS_SECTOR_SIZE];
  for (uint32_t s = 0; s < sectors; s++) {
    mem_zero(sector, SFS_SECTOR_SIZE);
    uint32_t copy = SFS_SECTOR_SIZE;
    uint32_t remain = len - (s * SFS_SECTOR_SIZE);
    if (remain < copy)
      copy = remain;
    for (uint32_t i = 0; i < copy; i++)
      sector[i] = src[s * SFS_SECTOR_SIZE + i];
    if (!sfs_write_sector(start_lba + s, sector))
      return 0;
  }

  g_table[idx].used = 1;
  str_copy(g_table[idx].name, name, SFS_NAME_MAX);
  g_table[idx].start_lba = start_lba;
  g_table[idx].size_bytes = len;

  g_sb.file_count = 0;
  for (uint32_t i = 0; i < SFS_MAX_FILES; ++i)
    if (g_table[i].used)
      ++g_sb.file_count;

  g_sb.next_free_lba = start_lba + sectors;
  if (!sfs_store_table())
    return 0;
  if (!sfs_store_superblock())
    return 0;

  return 1;
}

int simplefs_read_file(const char *name, void *out, uint32_t maxlen,
                       uint32_t *out_len) {
  if (!g_storage_ready || !name || !out)
    return 0;

  if (g_kind == STORAGE_FS_SFS2) {
    sfs2_stat_t info;
    if (!sfs2_stat_path(&g_sfs2, name, &info) ||
        info.type != SFS2_INODE_FILE || info.size_bytes > UINT32_MAX)
      return 0;
    if (out_len)
      *out_len = (uint32_t)info.size_bytes;
    uint32_t copied = 0;
    return sfs2_read_file_at(&g_sfs2, name, 0u, out, maxlen, &copied);
  }

  if (g_kind == STORAGE_FS_FAT32) {
    uint32_t idx = 0;
    if (!fat_find(name, &idx))
      return 0;

    uint32_t full_len = g_fat_files[idx].size_bytes;
    if (out_len)
      *out_len = full_len;
    uint32_t want = full_len;
    if (want > maxlen)
      want = maxlen;

    uint8_t *dst = (uint8_t *)out;
    uint32_t copied = 0;
    uint32_t cluster = g_fat_files[idx].first_cluster;
    uint32_t budget_clusters = 4096;
    while (copied < want && budget_clusters--) {
      uint32_t lba0 = fat_cluster_to_lba(cluster);
      for (uint32_t s = 0; s < (uint32_t)g_fat.sectors_per_cluster && copied < want;
           s++) {
        uint8_t sec[FAT_SECTOR_SIZE];
        if (!fat_read_sector(lba0 + s, sec))
          return 0;
        uint32_t copy = want - copied;
        if (copy > FAT_SECTOR_SIZE)
          copy = FAT_SECTOR_SIZE;
        for (uint32_t i = 0; i < copy; i++)
          dst[copied + i] = sec[i];
        copied += copy;
      }

      if (copied >= want)
        break;
      uint32_t next = 0;
      if (!fat_get_fat_entry(cluster, &next))
        return 0;
      if (next >= 0x0FFFFFF8u)
        break;
      if (next < 2u)
        return 0;
      cluster = next;
    }
    return 1;
  }

  if (g_kind == STORAGE_FS_SFS) {
    uint32_t idx = 0;
    if (!sfs_find(name, &idx))
      return 0;

    uint32_t len = g_table[idx].size_bytes;
    if (out_len)
      *out_len = len;
    if (len > maxlen)
      len = maxlen;

    uint32_t sectors =
        (g_table[idx].size_bytes + (SFS_SECTOR_SIZE - 1u)) / SFS_SECTOR_SIZE;
    uint8_t sector[SFS_SECTOR_SIZE];
    uint8_t *dst = (uint8_t *)out;

    uint32_t copied = 0;
    for (uint32_t s = 0; s < sectors && copied < len; s++) {
      if (!sfs_read_sector(g_table[idx].start_lba + s, sector))
        return 0;
      uint32_t copy = len - copied;
      if (copy > SFS_SECTOR_SIZE)
        copy = SFS_SECTOR_SIZE;
      for (uint32_t i = 0; i < copy; i++)
        dst[copied + i] = sector[i];
      copied += copy;
    }

    return 1;
  }

  return 0;
}


/* Read a bounded file range without allocating the entire file in the kernel. */
int simplefs_read_file_at(const char *name, uint32_t offset, void *out,
                          uint32_t maxlen, uint32_t *out_len) {
  if (out_len)
    *out_len = 0;
  if (!g_storage_ready || !name || (!out && maxlen != 0))
    return 0;

  if (g_kind == STORAGE_FS_SFS2) {
    sfs2_stat_t info;
    if (!sfs2_stat_path(&g_sfs2, name, &info) ||
        info.type != SFS2_INODE_FILE || info.size_bytes > UINT32_MAX)
      return 0;
    uint32_t copied = 0;
    if (!sfs2_read_file_at(&g_sfs2, name, offset, out, maxlen, &copied))
      return 0;
    if (out_len)
      *out_len = copied;
    return 1;
  }

  uint32_t file_size = 0;
  uint32_t idx = 0;
  if (g_kind == STORAGE_FS_SFS) {
    if (!sfs_find(name, &idx))
      return 0;
    file_size = g_table[idx].size_bytes;
  } else if (g_kind == STORAGE_FS_FAT32) {
    if (!fat_find(name, &idx))
      return 0;
    file_size = g_fat_files[idx].size_bytes;
  } else {
    return 0;
  }

  if (maxlen == 0 || offset >= file_size)
    return 1;

  uint32_t remaining = file_size - offset;
  uint32_t wanted = maxlen < remaining ? maxlen : remaining;
  uint32_t copied = 0;
  uint8_t sector[SFS_SECTOR_SIZE];
  uint8_t *dst = (uint8_t *)out;

  if (g_kind == STORAGE_FS_SFS) {
    while (copied < wanted) {
      uint32_t absolute = offset + copied;
      uint32_t sector_index = absolute / SFS_SECTOR_SIZE;
      uint32_t sector_offset = absolute % SFS_SECTOR_SIZE;
      if (!sfs_read_sector(g_table[idx].start_lba + sector_index, sector))
        return 0;
      uint32_t chunk = SFS_SECTOR_SIZE - sector_offset;
      if (chunk > wanted - copied)
        chunk = wanted - copied;
      for (uint32_t i = 0; i < chunk; ++i)
        dst[copied + i] = sector[sector_offset + i];
      copied += chunk;
    }
  } else {
    /* FAT32 stores file data in a cluster chain. Skip whole clusters first,
       then copy sector slices while following the chain with a hard budget. */
    uint32_t cluster_bytes =
        (uint32_t)g_fat.sectors_per_cluster * FAT_SECTOR_SIZE;
    if (cluster_bytes == 0)
      return 0;
    uint32_t cluster = g_fat_files[idx].first_cluster;
    if (cluster < 2u || cluster >= g_fat.cluster_count + 2u)
      return 0;

    uint32_t clusters_to_skip = offset / cluster_bytes;
    uint32_t within_cluster = offset % cluster_bytes;
    uint32_t budget_clusters = g_fat.cluster_count;
    while (clusters_to_skip > 0) {
      uint32_t next = 0;
      if (!budget_clusters-- || !fat_get_fat_entry(cluster, &next) ||
          next < 2u || next >= g_fat.cluster_count + 2u)
        return 0;
      cluster = next;
      --clusters_to_skip;
    }

    while (copied < wanted && budget_clusters--) {
      uint32_t first_sector = within_cluster / FAT_SECTOR_SIZE;
      uint32_t sector_offset = within_cluster % FAT_SECTOR_SIZE;
      uint32_t cluster_lba = fat_cluster_to_lba(cluster);
      for (uint32_t s = first_sector;
           s < (uint32_t)g_fat.sectors_per_cluster && copied < wanted; ++s) {
        if (!fat_read_sector(cluster_lba + s, sector))
          return 0;
        uint32_t chunk = FAT_SECTOR_SIZE - sector_offset;
        if (chunk > wanted - copied)
          chunk = wanted - copied;
        for (uint32_t i = 0; i < chunk; ++i)
          dst[copied + i] = sector[sector_offset + i];
        copied += chunk;
        sector_offset = 0;
      }
      within_cluster = 0;
      if (copied < wanted) {
        uint32_t next = 0;
        if (!fat_get_fat_entry(cluster, &next) ||
            next < 2u || next >= g_fat.cluster_count + 2u)
          return 0;
        cluster = next;
      }
    }
    if (copied != wanted)
      return 0;
  }

  if (out_len)
    *out_len = copied;
  return 1;
}

int simplefs_is_directory(const char *name) {
  if (!name || g_kind != STORAGE_FS_SFS2)
    return 0;
  sfs2_stat_t info;
  return sfs2_stat_path(&g_sfs2, name, &info) &&
         info.type == SFS2_INODE_DIRECTORY;
}

uint32_t simplefs_path_limit(void) {
  return g_kind == STORAGE_FS_SFS2 ? 127u : 31u;
}

int simplefs_is_mounted(void) {
  return g_kind != STORAGE_FS_NONE;
}

/* Writable formats are journaled SFS2 and legacy SFS1. FAT32 and legacy
   journal-less SFS2 volumes are intentionally mounted read-only. */
int simplefs_is_writable(void) {
  if (!g_storage_ready)
    return 0;
  if (g_kind == STORAGE_FS_SFS)
    return 1;
  if (g_kind == STORAGE_FS_SFS2)
    return g_sfs2.superblock.journal_lba != 0 &&
           g_sfs2.superblock.journal_sectors != 0;
  return 0;
}
