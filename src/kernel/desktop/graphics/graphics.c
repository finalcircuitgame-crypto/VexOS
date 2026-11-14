#include "graphics.h"

static BootInfo *g_boot_info;
static uint32_t *g_backbuffer;
static const uint8_t *g_font;

void gfx_init(BootInfo *boot_info, uint32_t *backbuffer, const uint8_t *font) {
  g_boot_info = boot_info;
  g_backbuffer = backbuffer;
  g_font = font;
}

void gfx_set_backbuffer(uint32_t *backbuffer) {
  g_backbuffer = backbuffer;
}

static uint32_t *gfx_target(void) {
  if (!g_boot_info)
    return 0;
  return g_backbuffer ? g_backbuffer : g_boot_info->framebuffer;
}

void gfx_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
  if (!g_boot_info || x >= g_boot_info->width || y >= g_boot_info->height)
    return;
  uint32_t *dst = gfx_target();
  if (dst)
    dst[y * g_boot_info->pitch + x] = color;
}

void gfx_draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) {
  if (!g_boot_info || x >= g_boot_info->width || y >= g_boot_info->height)
    return;
  uint32_t x2 = x + width, y2 = y + height;
  if (x2 < x || x2 > g_boot_info->width) x2 = g_boot_info->width;
  if (y2 < y || y2 > g_boot_info->height) y2 = g_boot_info->height;
  uint32_t *dst = gfx_target();
  if (!dst) return;
  for (uint32_t yy = y; yy < y2; ++yy) {
    uint32_t *row = &dst[yy * g_boot_info->pitch];
    for (uint32_t xx = x; xx < x2; ++xx) row[xx] = color;
  }
}

static uint32_t blend_rgb(uint32_t dst, uint32_t src, uint8_t alpha) {
  uint32_t inv = 255u - (uint32_t)alpha;
  uint32_t r = (((dst >> 16) & 255u) * inv + ((src >> 16) & 255u) * alpha) / 255u;
  uint32_t g = (((dst >> 8) & 255u) * inv + ((src >> 8) & 255u) * alpha) / 255u;
  uint32_t b = ((dst & 255u) * inv + (src & 255u) * alpha) / 255u;
  return (r << 16) | (g << 8) | b;
}

void gfx_blend_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color, uint8_t alpha) {
  if (!g_boot_info || !alpha || x >= g_boot_info->width || y >= g_boot_info->height)
    return;
  uint32_t x2 = x + width, y2 = y + height;
  if (x2 < x || x2 > g_boot_info->width) x2 = g_boot_info->width;
  if (y2 < y || y2 > g_boot_info->height) y2 = g_boot_info->height;
  uint32_t *dst = gfx_target();
  if (!dst) return;
  for (uint32_t yy = y; yy < y2; ++yy) {
    uint32_t *row = &dst[yy * g_boot_info->pitch];
    for (uint32_t xx = x; xx < x2; ++xx) row[xx] = blend_rgb(row[xx], color, alpha);
  }
}

void gfx_blend_round_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t radius, uint32_t color, uint8_t alpha) {
  if (!g_boot_info || !alpha || x >= g_boot_info->width || y >= g_boot_info->height)
    return;
  if (!radius) { gfx_blend_rect(x, y, width, height, color, alpha); return; }
  uint32_t x2 = x + width, y2 = y + height;
  if (x2 < x || x2 > g_boot_info->width) x2 = g_boot_info->width;
  if (y2 < y || y2 > g_boot_info->height) y2 = g_boot_info->height;
  uint32_t rr = radius * radius;
  uint32_t *dst = gfx_target();
  if (!dst) return;
  for (uint32_t yy = y; yy < y2; ++yy) {
    uint32_t *row = &dst[yy * g_boot_info->pitch];
    for (uint32_t xx = x; xx < x2; ++xx) {
      uint32_t inside = 1;
      if (xx < x + radius && yy < y + radius) {
        int32_t dx=(int32_t)xx-(int32_t)(x+radius), dy=(int32_t)yy-(int32_t)(y+radius);
        inside=(uint32_t)(dx*dx+dy*dy)<=rr;
      } else if (xx >= x2-radius && yy < y+radius) {
        int32_t dx=(int32_t)xx-(int32_t)(x2-radius-1u), dy=(int32_t)yy-(int32_t)(y+radius);
        inside=(uint32_t)(dx*dx+dy*dy)<=rr;
      } else if (xx < x+radius && yy >= y2-radius) {
        int32_t dx=(int32_t)xx-(int32_t)(x+radius), dy=(int32_t)yy-(int32_t)(y2-radius-1u);
        inside=(uint32_t)(dx*dx+dy*dy)<=rr;
      } else if (xx >= x2-radius && yy >= y2-radius) {
        int32_t dx=(int32_t)xx-(int32_t)(x2-radius-1u), dy=(int32_t)yy-(int32_t)(y2-radius-1u);
        inside=(uint32_t)(dx*dx+dy*dy)<=rr;
      }
      if (inside) row[xx]=blend_rgb(row[xx],color,alpha);
    }
  }
}

void gfx_draw_glyph(uint32_t x, uint32_t y, uint8_t character, uint32_t foreground) {
  if (!g_font) return;
  const uint8_t *glyph = &g_font[(uint32_t)character * 8u];
  for (uint32_t gy=0; gy<8u; ++gy) {
    uint8_t row=glyph[gy];
    for (uint32_t gx=0; gx<8u; ++gx)
      if (row & (uint8_t)(1u << (7u-gx))) gfx_put_pixel(x+gx,y+gy,foreground);
  }
}

void gfx_draw_string(uint32_t x, uint32_t y, const char *text, uint32_t foreground) {
  uint32_t cx=x;
  while (text && *text) {
    char ch=*text++;
    if (ch=='\n') { y+=8u; cx=x; continue; }
    if (ch=='\t') { cx+=32u; continue; }
    gfx_draw_glyph(cx,y,(uint8_t)ch,foreground);
    cx+=8u;
  }
}
