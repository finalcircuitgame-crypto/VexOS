#include "simplefs.h"
#include "ata_pio.h"
#include <stdint.h>

void serial_print(const char *str);

typedef enum {
  STORAGE_FS_NONE = 0,
  STORAGE_FS_SFS = 1,
  STORAGE_FS_FAT32 = 2,
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
static sfs_superblock_t g_sb;
static sfs_file_t g_table[SFS_MAX_FILES];

static storage_fs_kind_t g_kind = STORAGE_FS_NONE;

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

static int fat_read_sector(uint32_t lba, void *buf) {
  return ata_pio_read28(g_dev, lba, 1, buf);
}

static uint32_t fat_cluster_to_lba(uint32_t cluster) {
  if (cluster < 2u)
    cluster = 2u;
  return g_fat.data_start_lba + (cluster - 2u) * (uint32_t)g_fat.sectors_per_cluster;
}

static int fat_get_fat_entry(uint32_t cluster, uint32_t *out_next) {
  if (!out_next)
    return 0;
  uint32_t fat_off = cluster * 4u;
  uint32_t sec = g_fat.fat_start_lba + (fat_off / FAT_SECTOR_SIZE);
  uint32_t off = fat_off % FAT_SECTOR_SIZE;
  uint8_t buf[FAT_SECTOR_SIZE];
  if (!fat_read_sector(sec, buf))
    return 0;
  uint32_t v = rd32(&buf[off]);
  v &= 0x0FFFFFFFu;
  *out_next = v;
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
  g_fat.fat_size_sectors = rd32(&b[36]);
  g_fat.root_cluster = rd32(&b[44]);
  if (g_fat.bytes_per_sector != 512 || g_fat.sectors_per_cluster == 0 ||
      g_fat.num_fats == 0 || g_fat.fat_size_sectors == 0 || g_fat.root_cluster < 2u) {
    return 0;
  }

  g_fat.fat_start_lba = (uint32_t)g_fat.reserved_sector_count;
  g_fat.data_start_lba = g_fat.fat_start_lba +
                         (uint32_t)g_fat.num_fats * g_fat.fat_size_sectors;

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
    if (next < 2u)
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
  return ata_pio_read28(g_dev, lba, 1, buf);
}
static int sfs_write_sector(uint32_t lba, const void *buf) {
  return ata_pio_write28(g_dev, lba, 1, buf);
}

static int sfs_load_table(void) {
  uint8_t sector[SFS_SECTOR_SIZE];
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

int simplefs_file_size(const char *name, uint32_t *out_size) {
  if (!g_dev || !name)
    return 0;
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

static int sfs_format(void) {
  mem_zero(&g_sb, (uint32_t)sizeof(g_sb));
  mem_zero(&g_table[0], (uint32_t)sizeof(g_table));

  g_sb.magic = SFS_MAGIC;
  g_sb.version = SFS_VERSION;
  g_sb.table_lba = 1;
  g_sb.table_sectors = 8; // 8 * 512 = 4096 bytes table storage
  g_sb.data_lba = g_sb.table_lba + g_sb.table_sectors;
  g_sb.next_free_lba = g_sb.data_lba;
  g_sb.file_count = 0;

  if (!sfs_store_superblock())
    return 0;
  if (!sfs_store_table())
    return 0;

  serial_print("[SFS] formatted\n");
  return 1;
}

int simplefs_init(void) {
  if (!ata_pio_init()) {
    serial_print("[SFS] no disk\n");
    return 0;
  }

  g_dev = ata_pio_get_storage_device();
  if (!g_dev) {
    serial_print("[SFS] no storage device\n");
    return 0;
  }

  // Prefer FAT32 if detected on the storage disk.
  if (fat_mount_and_scan_root()) {
    g_kind = STORAGE_FS_FAT32;
    serial_print("[FAT32] mounted\n");
    return 1;
  }
  g_kind = STORAGE_FS_SFS;

  if (!sfs_load_superblock()) {
    serial_print("[SFS] read superblock failed\n");
    return 0;
  }

  if (g_sb.magic != SFS_MAGIC || g_sb.version != SFS_VERSION ||
      g_sb.table_sectors == 0 || g_sb.table_sectors > 64) {
    serial_print("[SFS] superblock invalid; formatting\n");
    if (!sfs_format())
      return 0;
  } else {
    if (!sfs_load_table()) {
      serial_print("[SFS] load table failed; formatting\n");
      if (!sfs_format())
        return 0;
    }
    serial_print("[SFS] mounted\n");
  }

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

uint32_t simplefs_file_count(void) {
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

int simplefs_write_file(const char *name, const void *data, uint32_t len) {
  if (g_kind == STORAGE_FS_FAT32) {
    // FAT32 write support not implemented yet.
    return 0;
  }
  if (!g_dev || !name || !data)
    return 0;
  if (str_len32(name) == 0 || str_len32(name) >= SFS_NAME_MAX)
    return 0;

  uint32_t idx = 0;
  if (!sfs_find(name, &idx)) {
    if (!sfs_alloc_entry(&idx))
      return 0;
  }

  uint32_t sectors = (len + (SFS_SECTOR_SIZE - 1u)) / SFS_SECTOR_SIZE;
  if (sectors == 0)
    sectors = 1;

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

  g_sb.next_free_lba = start_lba + sectors;
  if (!sfs_store_table())
    return 0;
  if (!sfs_store_superblock())
    return 0;

  return 1;
}

int simplefs_read_file(const char *name, void *out, uint32_t maxlen,
                       uint32_t *out_len) {
  if (!g_dev || !name || !out)
    return 0;

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

int simplefs_is_mounted(void) {
  return g_kind != STORAGE_FS_NONE;
}
