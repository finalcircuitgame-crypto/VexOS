#include "compositor.h"
#include "../../include/hw_profile.h"

static BootInfo *g_boot_info;
static uint8_t g_qemu_vga;
static uint8_t g_vbe_pageflip;
static uint32_t g_active_page;
static uint64_t g_page_pixels;

#define VBE_INDEX_PORT 0x01CEu
#define VBE_DATA_PORT  0x01CFu
#define VGA_STATUS_PORT 0x03DAu

#define VBE_INDEX_ID                 0x00u
#define VBE_INDEX_XRES               0x01u
#define VBE_INDEX_YRES               0x02u
#define VBE_INDEX_BPP                0x03u
#define VBE_INDEX_ENABLE             0x04u
#define VBE_INDEX_VIRT_WIDTH         0x06u
#define VBE_INDEX_VIRT_HEIGHT        0x07u
#define VBE_INDEX_Y_OFFSET           0x09u
#define VBE_INDEX_VIDEO_MEMORY_64K   0x0Au

#define VBE_ID_MIN                   0xB0C4u
#define VBE_ID_MAX                   0xB0C5u
#define VBE_ENABLED                  0x0001u
#define VGA_VERTICAL_RETRACE         0x08u

extern void serial_print(const char *str);

static inline void port_outw(uint16_t port, uint16_t value) {
  __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t port_inw(uint16_t port) {
  uint16_t value;
  __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

static inline uint8_t port_inb(uint16_t port) {
  uint8_t value;
  __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

static uint16_t vbe_read(uint16_t index) {
  port_outw(VBE_INDEX_PORT, index);
  return port_inw(VBE_DATA_PORT);
}

static void vbe_write(uint16_t index, uint16_t value) {
  port_outw(VBE_INDEX_PORT, index);
  port_outw(VBE_DATA_PORT, value);
}

/* Bounded polling avoids hanging the desktop if a virtual VGA implementation
   stops reporting retrace transitions. */
void compositor_wait_vsync(void) {
  if (!g_qemu_vga)
    return;

  uint32_t timeout = 1000000u;
  while ((port_inb(VGA_STATUS_PORT) & VGA_VERTICAL_RETRACE) && timeout) {
    --timeout;
    __asm__ volatile("pause");
  }

  timeout = 1000000u;
  while (!(port_inb(VGA_STATUS_PORT) & VGA_VERTICAL_RETRACE) && timeout) {
    --timeout;
    __asm__ volatile("pause");
  }
}

static void compositor_copy_rect(uint32_t *destination,
                                 const uint32_t *source,
                                 uint32_t x, uint32_t y,
                                 uint32_t width, uint32_t height) {
  if (!g_boot_info || !destination || !source ||
      x >= g_boot_info->width || y >= g_boot_info->height)
    return;

  if (width > g_boot_info->width - x)
    width = g_boot_info->width - x;
  if (height > g_boot_info->height - y)
    height = g_boot_info->height - y;

  for (uint32_t row = 0; row < height; ++row) {
    uint32_t *dst =
        &destination[(uint64_t)(y + row) * g_boot_info->pitch + x];
    const uint32_t *src =
        &source[(uint64_t)(y + row) * g_boot_info->pitch + x];
    for (uint32_t col = 0; col < width; ++col)
      dst[col] = src[col];
  }
}

void compositor_init(BootInfo *boot_info) {
  g_boot_info = boot_info;
  g_qemu_vga = 0;
  g_vbe_pageflip = 0;
  g_active_page = 0;
  g_page_pixels = 0;
}

/*
 * QEMU's standard VGA adapter (1234:1111) implements the Bochs VBE
 * DISPI interface. Its virtual framebuffer can hold two complete scanout
 * pages; changing Y_OFFSET during vertical retrace swaps the visible page
 * without copying pixels into the page currently being scanned out.
 *
 * UEFI GOP has no portable page-flip API, so this backend is deliberately
 * enabled only after identifying QEMU stdvga and validating the live mode,
 * pitch, and available VRAM.
 */
int compositor_enable_pageflip(void) {
  if (!g_boot_info || !g_boot_info->framebuffer || !g_boot_info->width ||
      !g_boot_info->height || g_boot_info->pitch < g_boot_info->width)
    return 0;

  const hw_profile_t *hw = hw_profile_get();
  if (!hw || !hw->has_pci_display ||
      hw->gpu_vendor_id != 0x1234u || hw->gpu_device_id != 0x1111u)
    return 0;

  g_qemu_vga = 1;

  uint16_t id = vbe_read(VBE_INDEX_ID);
  uint16_t enabled = vbe_read(VBE_INDEX_ENABLE);
  uint16_t xres = vbe_read(VBE_INDEX_XRES);
  uint16_t yres = vbe_read(VBE_INDEX_YRES);
  uint16_t bpp = vbe_read(VBE_INDEX_BPP);
  uint16_t video_memory_64k = vbe_read(VBE_INDEX_VIDEO_MEMORY_64K);

  if (id < VBE_ID_MIN || id > VBE_ID_MAX ||
      !(enabled & VBE_ENABLED) ||
      xres != g_boot_info->width || yres != g_boot_info->height ||
      bpp != 32u || g_boot_info->pitch > 0xFFFFu ||
      video_memory_64k == 0u || video_memory_64k == 0xFFFFu) {
    serial_print("[DISPLAY] QEMU VGA found, but its VBE mode is not page-flip compatible\n");
    return 0;
  }

  uint64_t page_pixels =
      (uint64_t)g_boot_info->pitch * (uint64_t)g_boot_info->height;
  if (page_pixels > UINT64_MAX / (2u * sizeof(uint32_t)))
    return 0;

  uint64_t required_bytes = page_pixels * 2u * sizeof(uint32_t);
  uint64_t video_memory_bytes = (uint64_t)video_memory_64k * 65536ull;
  if (required_bytes > video_memory_bytes) {
    serial_print("[DISPLAY] QEMU VGA has insufficient VRAM for two scanout pages\n");
    return 0;
  }

  vbe_write(VBE_INDEX_VIRT_WIDTH, (uint16_t)g_boot_info->pitch);
  uint16_t virtual_width = vbe_read(VBE_INDEX_VIRT_WIDTH);
  uint16_t virtual_height = vbe_read(VBE_INDEX_VIRT_HEIGHT);
  if (virtual_width < g_boot_info->pitch ||
      virtual_height < (uint32_t)g_boot_info->height * 2u) {
    serial_print("[DISPLAY] QEMU VGA rejected the requested virtual framebuffer size\n");
    return 0;
  }

  vbe_write(VBE_INDEX_Y_OFFSET, 0u);
  g_page_pixels = page_pixels;
  g_active_page = 0;
  g_vbe_pageflip = 1;
  serial_print("[DISPLAY] QEMU VBE double-buffered page flipping enabled\n");
  return 1;
}

int compositor_pageflip_active(void) {
  return g_vbe_pageflip != 0;
}

uint32_t *compositor_frontbuffer(void) {
  if (!g_boot_info || !g_boot_info->framebuffer)
    return 0;
  if (!g_vbe_pageflip)
    return g_boot_info->framebuffer;
  return g_boot_info->framebuffer + (g_active_page * g_page_pixels);
}

uint32_t *compositor_backbuffer(void) {
  if (!g_boot_info || !g_boot_info->framebuffer || !g_vbe_pageflip)
    return 0;
  return g_boot_info->framebuffer +
         ((g_active_page ^ 1u) * g_page_pixels);
}

void compositor_blit(uint32_t *destination, const uint32_t *source,
                     uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  compositor_copy_rect(destination, source, x, y, width, height);
}

void compositor_present_rect(const uint32_t *scene,
                             uint32_t x, uint32_t y,
                             uint32_t width, uint32_t height) {
  if (!g_boot_info || !g_boot_info->framebuffer || !scene)
    return;

  if (!g_vbe_pageflip) {
    compositor_wait_vsync();
    compositor_copy_rect(g_boot_info->framebuffer, scene,
                         x, y, width, height);
    return;
  }

  uint32_t next_page = g_active_page ^ 1u;
  uint32_t *next_frame =
      g_boot_info->framebuffer + (next_page * g_page_pixels);
  compositor_copy_rect(next_frame, scene, x, y, width, height);

  /* Switch only at vertical blank. The old page is now safe to update. */
  compositor_wait_vsync();
  vbe_write(VBE_INDEX_Y_OFFSET,
            (uint16_t)(next_page * g_boot_info->height));
  uint32_t old_page = g_active_page;
  g_active_page = next_page;

  uint32_t *old_frame =
      g_boot_info->framebuffer + (old_page * g_page_pixels);
  compositor_copy_rect(old_frame, scene, x, y, width, height);
}

void compositor_present(const uint32_t *scene, uint32_t width,
                        uint32_t height) {
  compositor_present_rect(scene, 0, 0, width, height);
}
