#include "graphics.h"
#include "font_atlas.h"

static BootInfo *g_boot_info;
static uint32_t *g_backbuffer;
static const uint8_t *g_font;
static uint8_t g_font_family = GFX_FONT_INTER;
static uint8_t g_font_scale = 1u;
static uint8_t g_font_bold;
static gfx_rect_t g_clip_stack[GFX_CLIP_STACK_MAX];
static uint32_t g_clip_depth;

static uint32_t gfx_min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t gfx_max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }

static uint32_t *gfx_target(void) {
  if (!g_boot_info)
    return 0;
  return g_backbuffer ? g_backbuffer : g_boot_info->framebuffer;
}

static gfx_rect_t gfx_screen_rect(void) {
  gfx_rect_t r = {0, 0, 0, 0};
  if (g_boot_info) {
    r.width = g_boot_info->width;
    r.height = g_boot_info->height;
  }
  return r;
}

static gfx_rect_t gfx_intersect(gfx_rect_t a, gfx_rect_t b) {
  uint32_t ax2 = a.x + a.width;
  uint32_t ay2 = a.y + a.height;
  uint32_t bx2 = b.x + b.width;
  uint32_t by2 = b.y + b.height;
  if (ax2 < a.x) ax2 = UINT32_MAX;
  if (ay2 < a.y) ay2 = UINT32_MAX;
  if (bx2 < b.x) bx2 = UINT32_MAX;
  if (by2 < b.y) by2 = UINT32_MAX;

  uint32_t x1 = gfx_max_u32(a.x, b.x);
  uint32_t y1 = gfx_max_u32(a.y, b.y);
  uint32_t x2 = gfx_min_u32(ax2, bx2);
  uint32_t y2 = gfx_min_u32(ay2, by2);
  gfx_rect_t out = {x1, y1, x2 > x1 ? x2 - x1 : 0,
                    y2 > y1 ? y2 - y1 : 0};
  return out;
}

static gfx_rect_t gfx_current_clip(void) {
  return g_clip_depth ? g_clip_stack[g_clip_depth - 1u] : gfx_screen_rect();
}

static uint32_t gfx_blend_rgb(uint32_t dst, uint32_t src, uint8_t alpha) {
  uint32_t inv = 255u - (uint32_t)alpha;
  uint32_t r = (((dst >> 16) & 255u) * inv +
                ((src >> 16) & 255u) * alpha) / 255u;
  uint32_t g = (((dst >> 8) & 255u) * inv +
                ((src >> 8) & 255u) * alpha) / 255u;
  uint32_t b = ((dst & 255u) * inv + (src & 255u) * alpha) / 255u;
  return (r << 16) | (g << 8) | b;
}

void gfx_init(BootInfo *boot_info, uint32_t *backbuffer, const uint8_t *font) {
  g_boot_info = boot_info;
  g_backbuffer = backbuffer;
  g_font = font;
  g_font_family = GFX_FONT_INTER;
  g_font_scale = 1u;
  g_font_bold = 0u;
  g_clip_depth = 0;
}

void gfx_set_backbuffer(uint32_t *backbuffer) { g_backbuffer = backbuffer; }
void gfx_reset_clip(void) { g_clip_depth = 0; }

int gfx_push_clip(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  if (g_clip_depth >= GFX_CLIP_STACK_MAX)
    return 0;
  gfx_rect_t requested = {x, y, width, height};
  gfx_rect_t parent = gfx_current_clip();
  g_clip_stack[g_clip_depth++] = gfx_intersect(parent, requested);
  return 1;
}

void gfx_pop_clip(void) {
  if (g_clip_depth)
    --g_clip_depth;
}

void gfx_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
  if (!g_boot_info || x >= g_boot_info->width || y >= g_boot_info->height)
    return;
  gfx_rect_t clip = gfx_current_clip();
  if (x < clip.x || y < clip.y || x - clip.x >= clip.width ||
      y - clip.y >= clip.height)
    return;
  uint32_t *dst = gfx_target();
  if (dst)
    dst[y * g_boot_info->pitch + x] = color & 0x00FFFFFFu;
}

void gfx_blend_pixel(uint32_t x, uint32_t y, uint32_t color, uint8_t alpha) {
  if (!alpha || !g_boot_info || x >= g_boot_info->width ||
      y >= g_boot_info->height)
    return;
  gfx_rect_t clip = gfx_current_clip();
  if (x < clip.x || y < clip.y || x - clip.x >= clip.width ||
      y - clip.y >= clip.height)
    return;
  uint32_t *dst = gfx_target();
  if (!dst)
    return;
  uint32_t *pixel = &dst[y * g_boot_info->pitch + x];
  *pixel = gfx_blend_rgb(*pixel, color, alpha);
}

static int gfx_clip_bounds(uint32_t x, uint32_t y, uint32_t width,
                           uint32_t height, uint32_t *x1, uint32_t *y1,
                           uint32_t *x2, uint32_t *y2) {
  if (!g_boot_info || !width || !height)
    return 0;
  uint32_t right = x + width;
  uint32_t bottom = y + height;
  if (right < x) right = UINT32_MAX;
  if (bottom < y) bottom = UINT32_MAX;
  gfx_rect_t clip = gfx_intersect(gfx_current_clip(), gfx_screen_rect());
  *x1 = gfx_max_u32(x, clip.x);
  *y1 = gfx_max_u32(y, clip.y);
  *x2 = gfx_min_u32(right, clip.x + clip.width);
  *y2 = gfx_min_u32(bottom, clip.y + clip.height);
  return *x1 < *x2 && *y1 < *y2;
}

void gfx_draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                   uint32_t color) {
  uint32_t x1, y1, x2, y2;
  if (!gfx_clip_bounds(x, y, width, height, &x1, &y1, &x2, &y2))
    return;
  uint32_t *dst = gfx_target();
  if (!dst)
    return;
  color &= 0x00FFFFFFu;
  for (uint32_t yy = y1; yy < y2; ++yy) {
    uint32_t *row = &dst[yy * g_boot_info->pitch];
    for (uint32_t xx = x1; xx < x2; ++xx)
      row[xx] = color;
  }
}

void gfx_blend_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                    uint32_t color, uint8_t alpha) {
  if (!alpha)
    return;
  if (alpha == 255u) {
    gfx_draw_rect(x, y, width, height, color);
    return;
  }

  uint32_t x1, y1, x2, y2;
  if (!gfx_clip_bounds(x, y, width, height, &x1, &y1, &x2, &y2))
    return;
  uint32_t *dst = gfx_target();
  if (!dst)
    return;
  for (uint32_t yy = y1; yy < y2; ++yy) {
    uint32_t *row = &dst[yy * g_boot_info->pitch];
    for (uint32_t xx = x1; xx < x2; ++xx)
      row[xx] = gfx_blend_rgb(row[xx], color, alpha);
  }
}

void gfx_blend_round_rect(uint32_t x, uint32_t y, uint32_t width,
                          uint32_t height, uint32_t radius, uint32_t color,
                          uint8_t alpha) {
  uint32_t x1, y1, x2, y2;
  if (!alpha || !gfx_clip_bounds(x, y, width, height, &x1, &y1, &x2, &y2))
    return;
  if (!radius) {
    gfx_blend_rect(x, y, width, height, color, alpha);
    return;
  }
  uint32_t limit = gfx_min_u32(width, height) / 2u;
  if (radius > limit) radius = limit;
  if (alpha == 255u) {
    color &= 0x00FFFFFFu;
  }

  const uint64_t radius_sq = (uint64_t)radius * radius;
  const uint32_t left_center = x + radius;
  const uint32_t right_center = x + width - radius - 1u;
  const uint32_t top_center = y + radius;
  const uint32_t bottom_center = y + height - radius - 1u;
  uint32_t *dst = gfx_target();
  if (!dst)
    return;

  /* Calculate one horizontal inset per row instead of testing every pixel
     with 64-bit distance math. The inner span is then a simple row fill/blend. */
  for (uint32_t yy = y1; yy < y2; ++yy) {
    uint32_t dy = 0;
    if (yy < top_center)
      dy = top_center - yy;
    else if (yy > bottom_center)
      dy = yy - bottom_center;

    uint32_t inset = 0;
    if (dy) {
      uint64_t remaining = radius_sq - (uint64_t)dy * dy;
      while ((uint64_t)(inset + 1u) * (inset + 1u) <= remaining)
        ++inset;
    } else {
      inset = radius;
    }

    uint32_t row_left = left_center - inset;
    uint32_t row_right = right_center + inset + 1u;
    if (row_left < x1) row_left = x1;
    if (row_right > x2) row_right = x2;
    if (row_left >= row_right)
      continue;

    uint32_t *row = &dst[yy * g_boot_info->pitch];
    if (alpha == 255u) {
      for (uint32_t xx = row_left; xx < row_right; ++xx)
        row[xx] = color;
    } else {
      for (uint32_t xx = row_left; xx < row_right; ++xx)
        row[xx] = gfx_blend_rgb(row[xx], color, alpha);
    }
  }
}

void gfx_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                   uint32_t color) {
  int32_t dx = x1 >= x0 ? x1 - x0 : x0 - x1;
  int32_t sx = x0 < x1 ? 1 : -1;
  int32_t dy = y1 >= y0 ? y0 - y1 : y1 - y0;
  int32_t sy = y0 < y1 ? 1 : -1;
  int32_t error = dx + dy;
  for (;;) {
    if (x0 >= 0 && y0 >= 0)
      gfx_put_pixel((uint32_t)x0, (uint32_t)y0, color);
    if (x0 == x1 && y0 == y1)
      break;
    int32_t twice = error * 2;
    if (twice >= dy) { error += dy; x0 += sx; }
    if (twice <= dx) { error += dx; y0 += sy; }
  }
}

void gfx_blit_scaled(const uint32_t *source, uint32_t source_width,
                     uint32_t source_height, uint32_t source_stride,
                     uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  if (!source || !source_width || !source_height ||
      source_stride < source_width || !width || !height)
    return;

  uint32_t x1, y1, x2, y2;
  if (!gfx_clip_bounds(x, y, width, height, &x1, &y1, &x2, &y2))
    return;

  uint32_t *dst = gfx_target();
  if (!dst)
    return;

  /* Exact nearest-neighbour stepping without a 64-bit divide per pixel. */
  const uint32_t sx_step = source_width / width;
  const uint64_t sx_step_rem = source_width % width;
  const uint32_t sy_step = source_height / height;
  const uint64_t sy_step_rem = source_height % height;
  const uint64_t x_start = (uint64_t)(x1 - x) * source_width;
  const uint32_t sx_start = (uint32_t)(x_start / width);
  const uint64_t sx_start_rem = x_start % width;
  const uint64_t y_start = (uint64_t)(y1 - y) * source_height;
  uint32_t sy = (uint32_t)(y_start / height);
  uint64_t sy_rem = y_start % height;

  for (uint32_t dy = y1; dy < y2; ++dy) {
    uint32_t sx = sx_start;
    uint64_t sx_rem = sx_start_rem;
    uint32_t *out = &dst[dy * g_boot_info->pitch];
    const uint32_t *in = &source[sy * source_stride];

    for (uint32_t dx = x1; dx < x2; ++dx) {
      out[dx] = in[sx] & 0x00FFFFFFu;
      sx += sx_step;
      sx_rem += sx_step_rem;
      if (sx_rem >= width) {
        sx_rem -= width;
        ++sx;
      }
    }

    sy += sy_step;
    sy_rem += sy_step_rem;
    if (sy_rem >= height) {
      sy_rem -= height;
      ++sy;
    }
  }
}

void gfx_blit_rgba_scaled(const uint8_t *source, uint32_t source_width,
                          uint32_t source_height, uint32_t source_stride,
                          uint32_t x, uint32_t y, uint32_t width,
                          uint32_t height) {
  if (!source || !source_width || !source_height ||
      source_stride < source_width || !width || !height)
    return;

  uint32_t x1, y1, x2, y2;
  if (!gfx_clip_bounds(x, y, width, height, &x1, &y1, &x2, &y2))
    return;

  uint32_t *dst = gfx_target();
  if (!dst)
    return;

  /* Match gfx_blit_scaled's exact floor mapping using quotient/remainder
     accumulators; this keeps alpha blending from paying per-pixel divides. */
  const uint32_t sx_step = source_width / width;
  const uint64_t sx_step_rem = source_width % width;
  const uint32_t sy_step = source_height / height;
  const uint64_t sy_step_rem = source_height % height;
  const uint64_t y_start = (uint64_t)(y1 - y) * source_height;
  uint32_t sy = (uint32_t)(y_start / height);
  uint64_t sy_rem = y_start % height;

  for (uint32_t dy = y1; dy < y2; ++dy) {
    uint32_t sx = (uint32_t)(((uint64_t)(x1 - x) * source_width) / width);
    uint64_t sx_rem = ((uint64_t)(x1 - x) * source_width) % width;
    uint32_t *out = &dst[dy * g_boot_info->pitch];

    for (uint32_t dx = x1; dx < x2; ++dx) {
      const uint8_t *pixel =
          source + (((uint64_t)sy * source_stride + sx) * 4ull);
      uint8_t alpha = pixel[3];
      if (alpha) {
        uint32_t color = ((uint32_t)pixel[0] << 16) |
                         ((uint32_t)pixel[1] << 8) |
                         (uint32_t)pixel[2];
        if (alpha == 255u)
          out[dx] = color;
        else
          out[dx] = gfx_blend_rgb(out[dx], color, alpha);
      }

      sx += sx_step;
      sx_rem += sx_step_rem;
      if (sx_rem >= width) {
        sx_rem -= width;
        ++sx;
      }
    }

    sy += sy_step;
    sy_rem += sy_step_rem;
    if (sy_rem >= height) {
      sy_rem -= height;
      ++sy;
    }
  }
}

void gfx_set_font(gfx_font_family_t family, uint8_t scale, uint8_t bold) {
  if ((uint32_t)family >= VEXOS_FONT_FAMILY_COUNT)
    family = GFX_FONT_INTER;
  if (scale < 1u) scale = 1u;
  if (scale > 3u) scale = 3u;
  g_font_family = (uint8_t)family;
  g_font_scale = scale;
  g_font_bold = bold ? 1u : 0u;
}

gfx_font_family_t gfx_get_font_family(void) {
  return (gfx_font_family_t)g_font_family;
}

uint8_t gfx_get_font_scale(void) { return g_font_scale; }
uint8_t gfx_get_font_bold(void) { return g_font_bold; }

void gfx_draw_glyph(uint32_t x, uint32_t y, uint8_t character,
                    uint32_t foreground) {
  if (!g_boot_info)
    return;

  /* The new grayscale atlas is the normal path. Keep the original CGA font
     as a defensive fallback for non-ASCII bytes or an unavailable atlas. */
  if (character < VEXOS_FONT_GLYPH_COUNT) {
    const uint8_t *glyph =
        &vexos_font_atlas[g_font_family][g_font_bold][character][0];
    uint32_t scale = g_font_scale;
    uint32_t width = 8u * scale;
    uint32_t height = 8u * scale;
    gfx_rect_t clip = gfx_intersect(gfx_current_clip(), gfx_screen_rect());
    uint32_t clip_right = clip.x + clip.width;
    uint32_t clip_bottom = clip.y + clip.height;
    if (x >= clip_right || y >= clip_bottom ||
        clip.x >= x + width || clip.y >= y + height)
      return;

    uint32_t gx_start = clip.x > x ? (clip.x - x) / scale : 0u;
    uint32_t gy_start = clip.y > y ? (clip.y - y) / scale : 0u;
    uint32_t gx_end = clip_right > x ? (clip_right - x + scale - 1u) / scale : 0u;
    uint32_t gy_end = clip_bottom > y ? (clip_bottom - y + scale - 1u) / scale : 0u;
    if (gx_end > 8u) gx_end = 8u;
    if (gy_end > 8u) gy_end = 8u;

    uint32_t *dst = gfx_target();
    if (!dst)
      return;
    foreground &= 0x00FFFFFFu;
    for (uint32_t gy = gy_start; gy < gy_end; ++gy) {
      for (uint32_t gx = gx_start; gx < gx_end; ++gx) {
        uint8_t alpha = glyph[gy * 8u + gx];
        if (!alpha)
          continue;
        uint32_t px0 = x + gx * scale;
        uint32_t py0 = y + gy * scale;
        for (uint32_t sy = 0; sy < scale; ++sy) {
          uint32_t py = py0 + sy;
          if (py < clip.y || py >= clip_bottom)
            continue;
          uint32_t *row = &dst[py * g_boot_info->pitch];
          for (uint32_t sx = 0; sx < scale; ++sx) {
            uint32_t px = px0 + sx;
            if (px >= clip.x && px < clip_right)
              row[px] = gfx_blend_rgb(row[px], foreground, alpha);
          }
        }
      }
    }
    return;
  }

  if (!g_font)
    return;
  gfx_rect_t clip = gfx_intersect(gfx_current_clip(), gfx_screen_rect());
  uint32_t clip_right = clip.x + clip.width;
  uint32_t clip_bottom = clip.y + clip.height;
  if (x >= clip_right || y >= clip_bottom ||
      clip.x >= x + 8u || clip.y >= y + 8u)
    return;
  uint32_t gx_start = clip.x > x ? clip.x - x : 0u;
  uint32_t gy_start = clip.y > y ? clip.y - y : 0u;
  uint32_t gx_end = clip_right > x ? clip_right - x : 0u;
  uint32_t gy_end = clip_bottom > y ? clip_bottom - y : 0u;
  if (gx_end > 8u) gx_end = 8u;
  if (gy_end > 8u) gy_end = 8u;
  uint32_t *dst = gfx_target();
  if (!dst)
    return;
  const uint8_t *legacy_glyph = &g_font[(uint32_t)character * 8u];
  foreground &= 0x00FFFFFFu;
  for (uint32_t gy = gy_start; gy < gy_end; ++gy) {
    uint8_t bits = legacy_glyph[gy];
    uint32_t *out = &dst[(y + gy) * g_boot_info->pitch];
    for (uint32_t gx = gx_start; gx < gx_end; ++gx) {
      if (bits & (uint8_t)(1u << (7u - gx)))
        out[x + gx] = foreground;
    }
  }
}

void gfx_draw_string(uint32_t x, uint32_t y, const char *text,
                     uint32_t foreground) {
  uint32_t cx = x;
  uint32_t advance = 8u * g_font_scale;
  while (text && *text) {
    char ch = *text++;
    if (ch == '\n') { y += advance; cx = x; continue; }
    if (ch == '\t') { cx += advance * 4u; continue; }
    gfx_draw_glyph(cx, y, (uint8_t)ch, foreground);
    cx += advance;
  }
}
