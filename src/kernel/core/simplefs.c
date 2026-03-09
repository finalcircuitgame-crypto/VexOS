#include "simplefs.h"
#include "ata_pio.h"
#include <stdint.h>

void serial_print(const char *str);

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
  uint32_t idx = 0;
  if (!sfs_find(name, &idx))
    return 0;
  if (out_size)
    *out_size = g_table[idx].size_bytes;
  return 1;
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
  uint32_t n = 0;
  for (uint32_t i = 0; i < SFS_MAX_FILES; i++) {
    if (g_table[i].used)
      n++;
  }
  return n;
}

int simplefs_file_name_at(uint32_t index, char *out_name, uint32_t out_cap) {
  if (!out_name || out_cap == 0)
    return 0;

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

  uint32_t idx = 0;
  if (!sfs_find(name, &idx))
    return 0;

  uint32_t len = g_table[idx].size_bytes;
  if (out_len)
    *out_len = len;
  if (len > maxlen)
    len = maxlen;

  uint32_t sectors = (g_table[idx].size_bytes + (SFS_SECTOR_SIZE - 1u)) / SFS_SECTOR_SIZE;
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
