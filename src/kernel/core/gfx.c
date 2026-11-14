#include "../../include/gfx.h"

#include <stddef.h>
#include <stdint.h>

extern uint8_t _binary_CGA_F08_start[];

/* The current desktop uses a 256-glyph, 8x8 CGA font. */
#define GFX_GLYPH_WIDTH 8u
#define GFX_GLYPH_HEIGHT 8u

static uint32_t *gfx_resolve_target(const BootInfo *surface,
                                    uint32_t *target) {
  if (!surface || !surface->framebuffer || surface->width == 0u ||
      surface->height == 0u || surface->pitch < surface->width) {
    return NULL;
  }
  return target ? target : surface->framebuffer;
}

static int gfx_clip_rect(const BootInfo *surface, uint32_t *x, uint32_t *y,
                         uint32_t *width, uint32_t *height) {
  if (!surface || !x || !y || !width || !height ||
      *x >= surface->width || *y >= surface->height || *width == 0u ||
      *height == 0u) {
    return 0;
  }

  /* Subtraction-based clipping avoids x + width / y + height overflow. */
  if (*width > surface->width - *x)
    *width = surface->width - *x;
  if (*height > surface->height - *y)
    *height = surface->height - *y;

  return *width != 0u && *height != 0u;
}

static uint32_t gfx_blend_rgb(uint32_t destination, uint32_t source,
                              uint8_t alpha) {
  if (alpha == 0u)
    return destination;
  if (alpha == 255u)
    return source & 0x00FFFFFFu;

  uint32_t inverse = 255u - (uint32_t)alpha;
  uint32_t dr = (destination >> 16) & 0xFFu;
  uint32_t dg = (destination >> 8) & 0xFFu;
  uint32_t db = destination & 0xFFu;
  uint32_t sr = (source >> 16) & 0xFFu;
  uint32_t sg = (source >> 8) & 0xFFu;
  uint32_t sb = source & 0xFFu;

  uint32_t red = (dr * inverse + sr * (uint32_t)alpha) / 255u;
  uint32_t green = (dg * inverse + sg * (uint32_t)alpha) / 255u;
  uint32_t blue = (db * inverse + sb * (uint32_t)alpha) / 255u;
  return (red << 16) | (green << 8) | blue;
}

void gfx_put_pixel(const BootInfo *surface, uint32_t *target, uint32_t x,
                   uint32_t y, uint32_t color) {
  uint32_t *pixels = gfx_resolve_target(surface, target);
  if (!pixels || x >= surface->width || y >= surface->height)
    return;

  pixels[(uint64_t)y * surface->pitch + x] = color;
}

void gfx_fill_rect(const BootInfo *surface, uint32_t *target, uint32_t x,
                   uint32_t y, uint32_t width, uint32_t height,
                   uint32_t color) {
  uint32_t *pixels = gfx_resolve_target(surface, target);
  if (!pixels || !gfx_clip_rect(surface, &x, &y, &width, &height))
    return;

  for (uint32_t row_index = 0; row_index < height; ++row_index) {
    uint32_t *row =
        &pixels[(uint64_t)(y + row_index) * surface->pitch + x];
    for (uint32_t column = 0; column < width; ++column)
      row[column] = color;
  }
}

void gfx_blend_rect(const BootInfo *surface, uint32_t *target, uint32_t x,
                    uint32_t y, uint32_t width, uint32_t height,
                    uint32_t color, uint8_t alpha) {
  uint32_t *pixels = gfx_resolve_target(surface, target);
  if (!pixels || alpha == 0u ||
      !gfx_clip_rect(surface, &x, &y, &width, &height)) {
    return;
  }

  if (alpha == 255u) {
    gfx_fill_rect(surface, pixels, x, y, width, height,
                  color & 0x00FFFFFFu);
    return;
  }

  for (uint32_t row_index = 0; row_index < height; ++row_index) {
    uint32_t *row =
        &pixels[(uint64_t)(y + row_index) * surface->pitch + x];
    for (uint32_t column = 0; column < width; ++column)
      row[column] = gfx_blend_rgb(row[column], color, alpha);
  }
}

void gfx_blend_round_rect(const BootInfo *surface, uint32_t *target,
                          uint32_t x, uint32_t y, uint32_t width,
                          uint32_t height, uint32_t radius, uint32_t color,
                          uint8_t alpha) {
  uint32_t *pixels = gfx_resolve_target(surface, target);
  if (!pixels || alpha == 0u ||
      !gfx_clip_rect(surface, &x, &y, &width, &height)) {
    return;
  }

  uint32_t max_radius = (width < height ? width : height) / 2u;
  if (radius > max_radius)
    radius = max_radius;
  if (radius == 0u) {
    gfx_blend_rect(surface, pixels, x, y, width, height, color, alpha);
    return;
  }

  uint32_t right = x + width;
  uint32_t bottom = y + height;
  uint64_t radius_squared = (uint64_t)radius * radius;

  for (uint32_t py = y; py < bottom; ++py) {
    uint32_t *row = &pixels[(uint64_t)py * surface->pitch];
    for (uint32_t px = x; px < right; ++px) {
      int32_t dx = 0;
      int32_t dy = 0;
      int in_corner = 0;

      if (px < x + radius && py < y + radius) {
        dx = (int32_t)px - (int32_t)(x + radius);
        dy = (int32_t)py - (int32_t)(y + radius);
        in_corner = 1;
      } else if (px >= right - radius && py < y + radius) {
        dx = (int32_t)px - (int32_t)(right - radius - 1u);
        dy = (int32_t)py - (int32_t)(y + radius);
        in_corner = 1;
      } else if (px < x + radius && py >= bottom - radius) {
        dx = (int32_t)px - (int32_t)(x + radius);
        dy = (int32_t)py - (int32_t)(bottom - radius - 1u);
        in_corner = 1;
      } else if (px >= right - radius && py >= bottom - radius) {
        dx = (int32_t)px - (int32_t)(right - radius - 1u);
        dy = (int32_t)py - (int32_t)(bottom - radius - 1u);
        in_corner = 1;
      }

      if (!in_corner ||
          ((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy) <=
           radius_squared)) {
        row[px] = gfx_blend_rgb(row[px], color, alpha);
      }
    }
  }
}

void gfx_draw_glyph8(const BootInfo *surface, uint32_t *target, uint32_t x,
                     uint32_t y, uint8_t character, uint32_t foreground) {
  uint32_t *pixels = gfx_resolve_target(surface, target);
  if (!pixels || x >= surface->width || y >= surface->height)
    return;

  const uint8_t *font = _binary_CGA_F08_start;
  const uint8_t *glyph = &font[(uint32_t)character * GFX_GLYPH_HEIGHT];
  for (uint32_t row = 0; row < GFX_GLYPH_HEIGHT; ++row) {
    uint32_t py = y + row;
    if (py >= surface->height)
      break;

    uint8_t bits = glyph[row];
    for (uint32_t column = 0; column < GFX_GLYPH_WIDTH; ++column) {
      uint32_t px = x + column;
      if (px >= surface->width)
        break;
      if (bits & (uint8_t)(1u << (7u - column)))
        pixels[(uint64_t)py * surface->pitch + px] = foreground;
    }
  }
}

void gfx_draw_text8(const BootInfo *surface, uint32_t *target, uint32_t x,
                    uint32_t y, const char *text, uint32_t foreground) {
  uint32_t cursor_x = x;
  while (text && *text) {
    uint8_t character = (uint8_t)*text++;
    if (character == (uint8_t)'\n') {
      if (y > UINT32_MAX - GFX_GLYPH_HEIGHT)
        break;
      y += GFX_GLYPH_HEIGHT;
      cursor_x = x;
      continue;
    }
    if (character == (uint8_t)'\t') {
      if (cursor_x > UINT32_MAX - 32u)
        break;
      cursor_x += 32u;
      continue;
    }

    gfx_draw_glyph8(surface, target, cursor_x, y, character, foreground);
    if (cursor_x > UINT32_MAX - GFX_GLYPH_WIDTH)
      break;
    cursor_x += GFX_GLYPH_WIDTH;
    if (surface && cursor_x >= surface->width && x < surface->width)
      break;
  }
}

void gfx_blit_rect(const BootInfo *surface, uint32_t *destination,
                   const uint32_t *source, uint32_t x, uint32_t y,
                   uint32_t width, uint32_t height) {
  uint32_t *dst = gfx_resolve_target(surface, destination);
  if (!dst || !source ||
      !gfx_clip_rect(surface, &x, &y, &width, &height)) {
    return;
  }

  for (uint32_t row_index = 0; row_index < height; ++row_index) {
    uint32_t *dst_row =
        &dst[(uint64_t)(y + row_index) * surface->pitch + x];
    const uint32_t *src_row =
        &source[(uint64_t)(y + row_index) * surface->pitch + x];
    for (uint32_t column = 0; column < width; ++column)
      dst_row[column] = src_row[column];
  }
}

void gfx_blit_frame(const BootInfo *surface, const uint32_t *source,
                    uint32_t source_pitch_pixels, uint32_t source_width,
                    uint32_t source_height) {
  if (!surface || !source || !surface->framebuffer ||
      surface->width == 0u || surface->height == 0u ||
      surface->pitch < surface->width || source_pitch_pixels == 0u ||
      source_width == 0u || source_height == 0u) {
    return;
  }

  uint32_t copy_width = source_width < surface->width
                            ? source_width
                            : surface->width;
  uint32_t copy_height = source_height < surface->height
                             ? source_height
                             : surface->height;
  if (source_pitch_pixels < copy_width)
    return;

  for (uint32_t y = 0; y < copy_height; ++y) {
    uint32_t *dst_row =
        &surface->framebuffer[(uint64_t)y * surface->pitch];
    const uint32_t *src_row =
        &source[(uint64_t)y * source_pitch_pixels];
    for (uint32_t x = 0; x < copy_width; ++x)
      dst_row[x] = src_row[x];
  }
}


#ifdef VEXOS_CI_INPUT_TEST
int gfx_ci_self_test(void) {
  uint32_t framebuffer[16];
  for (uint32_t i = 0; i < 16u; ++i)
    framebuffer[i] = 0u;

  BootInfo surface;
  surface.framebuffer = framebuffer;
  surface.width = 4u;
  surface.height = 4u;
  surface.pitch = 4u;
  surface.LargestFreeRegion.Base = 0u;
  surface.LargestFreeRegion.Size = 0u;

  /* A deliberately oversized rectangle must clip without wrapping. */
  gfx_fill_rect(&surface, NULL, 2u, 2u, UINT32_MAX, UINT32_MAX,
                0x00112233u);
  if (framebuffer[10] != 0x00112233u ||
      framebuffer[11] != 0x00112233u ||
      framebuffer[14] != 0x00112233u ||
      framebuffer[15] != 0x00112233u ||
      framebuffer[9] != 0u) {
    return 0;
  }

  gfx_blend_rect(&surface, NULL, 0u, 0u, 1u, 1u, 0x00FF0000u, 128u);
  if (framebuffer[0] != 0x00800000u)
    return 0;
  gfx_blend_rect(&surface, NULL, 3u, 0u, 1u, 1u, 0xFFFFFFFFu, 255u);
  if (framebuffer[3] != 0x00FFFFFFu)
    return 0;

  gfx_blend_round_rect(&surface, NULL, 0u, 0u, 4u, 4u, 1u,
                       0x0000FF00u, 255u);
  if (framebuffer[10] != 0x0000FF00u ||
      framebuffer[0] != 0x00800000u) {
    return 0;
  }

  uint32_t source[16];
  for (uint32_t i = 0; i < 16u; ++i)
    source[i] = 0x000000A5u + i;
  gfx_blit_rect(&surface, framebuffer, source, 1u, 1u, 2u, 2u);
  if (framebuffer[5] != source[5] || framebuffer[6] != source[6] ||
      framebuffer[9] != source[9] || framebuffer[10] != source[10]) {
    return 0;
  }

  uint32_t small_source[4] = {
      0x00101010u, 0x00202020u, 0x00303030u, 0x00404040u};
  gfx_blit_frame(&surface, small_source, 2u, 2u, 2u);
  if (framebuffer[0] != small_source[0] ||
      framebuffer[1] != small_source[1] ||
      framebuffer[4] != small_source[2] ||
      framebuffer[5] != small_source[3] ||
      framebuffer[2] != 0x0000FF00u) {
    return 0;
  }

  return 1;
}
#endif
