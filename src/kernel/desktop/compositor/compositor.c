#include "compositor.h"

static BootInfo *g_boot_info;

void compositor_init(BootInfo *boot_info) {
  g_boot_info = boot_info;
}

void compositor_blit(uint32_t *destination, const uint32_t *source,
                     uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  if (!g_boot_info || !destination || !source)
    return;
  if (x >= g_boot_info->width || y >= g_boot_info->height)
    return;
  if (width > g_boot_info->width - x)
    width = g_boot_info->width - x;
  if (height > g_boot_info->height - y)
    height = g_boot_info->height - y;

  for (uint32_t row = 0; row < height; ++row) {
    uint32_t *dst = &destination[(y + row) * g_boot_info->pitch + x];
    const uint32_t *src = &source[(y + row) * g_boot_info->pitch + x];
    for (uint32_t col = 0; col < width; ++col)
      dst[col] = src[col];
  }
}

void compositor_present(const uint32_t *scene, uint32_t width, uint32_t height) {
  if (!g_boot_info || !g_boot_info->framebuffer || !scene)
    return;
  compositor_blit(g_boot_info->framebuffer, scene, 0, 0, width, height);
}
