#ifndef VEXOS_GRAPHICS_H
#define VEXOS_GRAPHICS_H

#include <stdint.h>
#include "../../../include/bootinfo.h"

/*
 * VexOS software renderer.
 *
 * Coordinates are framebuffer pixels. Colors are 0x00RRGGBB. Clip rectangles
 * are screen-space and use a half-open [x, x + width) / [y, y + height) range.
 */
#define GFX_CLIP_STACK_MAX 8u

typedef struct {
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
} gfx_rect_t;

void gfx_init(BootInfo *boot_info, uint32_t *backbuffer, const uint8_t *font);
void gfx_set_backbuffer(uint32_t *backbuffer);

void gfx_reset_clip(void);
int gfx_push_clip(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void gfx_pop_clip(void);

void gfx_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void gfx_blend_pixel(uint32_t x, uint32_t y, uint32_t color, uint8_t alpha);
void gfx_draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                   uint32_t color);
void gfx_blend_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                    uint32_t color, uint8_t alpha);
void gfx_blend_round_rect(uint32_t x, uint32_t y, uint32_t width,
                          uint32_t height, uint32_t radius, uint32_t color,
                          uint8_t alpha);
void gfx_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                   uint32_t color);

/* Nearest-neighbour image scaling. Source and destination are opaque RGB. */
void gfx_blit_scaled(const uint32_t *source, uint32_t source_width,
                     uint32_t source_height, uint32_t source_stride,
                     uint32_t x, uint32_t y, uint32_t width, uint32_t height);

void gfx_draw_glyph(uint32_t x, uint32_t y, uint8_t character,
                    uint32_t foreground);
void gfx_draw_string(uint32_t x, uint32_t y, const char *text,
                     uint32_t foreground);

#endif
