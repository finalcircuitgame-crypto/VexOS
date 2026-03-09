#include "ramfs.h"
#include "heap.h"
#include <stdint.h>

typedef struct {
  uint8_t used;
  char name[32];
  void *data;
  uint32_t size;
} ramfs_entry_t;

static ramfs_entry_t g_ramfs[32];
static uint8_t g_inited;

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

static void str_copy(char *dst, const char *src, uint32_t cap) {
  if (!dst || cap == 0)
    return;
  uint32_t i = 0;
  if (src) {
    for (; i + 1u < cap && src[i]; i++)
      dst[i] = src[i];
  }
  dst[i] = 0;
}

int ramfs_init(void) {
  for (uint32_t i = 0; i < 32; i++) {
    g_ramfs[i].used = 0;
    g_ramfs[i].name[0] = 0;
    g_ramfs[i].data = 0;
    g_ramfs[i].size = 0;
  }
  g_inited = 1;
  return 1;
}

int ramfs_put_copy(const char *name, const void *data, uint32_t size) {
  if (!g_inited)
    ramfs_init();
  if (!name || !data)
    return 0;
  uint32_t nlen = str_len32(name);
  if (nlen == 0 || nlen >= 32)
    return 0;

  uint32_t slot = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < 32; i++) {
    if (g_ramfs[i].used && str_eq(g_ramfs[i].name, name)) {
      slot = i;
      break;
    }
  }
  if (slot == 0xFFFFFFFFu) {
    for (uint32_t i = 0; i < 32; i++) {
      if (!g_ramfs[i].used) {
        slot = i;
        break;
      }
    }
  }
  if (slot == 0xFFFFFFFFu)
    return 0;

  void *buf = kmalloc(size);
  if (!buf)
    return 0;
  const uint8_t *src = (const uint8_t *)data;
  uint8_t *dst = (uint8_t *)buf;
  for (uint32_t i = 0; i < size; i++)
    dst[i] = src[i];

  g_ramfs[slot].used = 1;
  str_copy(g_ramfs[slot].name, name, 32);
  g_ramfs[slot].data = buf;
  g_ramfs[slot].size = size;
  return 1;
}

int ramfs_get(const char *name, const void **out_data, uint32_t *out_size) {
  if (!g_inited)
    return 0;
  if (!name)
    return 0;

  for (uint32_t i = 0; i < 32; i++) {
    if (g_ramfs[i].used && str_eq(g_ramfs[i].name, name)) {
      if (out_data)
        *out_data = g_ramfs[i].data;
      if (out_size)
        *out_size = g_ramfs[i].size;
      return 1;
    }
  }
  return 0;
}
