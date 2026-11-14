#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include "bootinfo.h"

/*
 * VexOS framebuffer drawing primitives.
 *
 * Coordinates and dimensions are measured in pixels. BootInfo.pitch is the
 * number of uint32_t pixels per row, not a byte count. A NULL target selects
 * the hardware framebuffer; a non-NULL target selects a compatible
 * pitch-strided backbuffer.
 */

void gfx_put_pixel(const BootInfo *surface, uint32_t *target, uint32_t x,
                   uint32_t y, uint32_t color);

void gfx_fill_rect(const BootInfo *surface, uint32_t *target, uint32_t x,
                   uint32_t y, uint32_t width, uint32_t height,
                   uint32_t color);

void gfx_blend_rect(const BootInfo *surface, uint32_t *target, uint32_t x,
                    uint32_t y, uint32_t width, uint32_t height,
                    uint32_t color, uint8_t alpha);

void gfx_blend_round_rect(const BootInfo *surface, uint32_t *target,
                          uint32_t x, uint32_t y, uint32_t width,
                          uint32_t height, uint32_t radius, uint32_t color,
                          uint8_t alpha);

void gfx_draw_glyph8(const BootInfo *surface, uint32_t *target, uint32_t x,
                     uint32_t y, uint8_t character, uint32_t foreground);

void gfx_draw_text8(const BootInfo *surface, uint32_t *target, uint32_t x,
                    uint32_t y, const char *text, uint32_t foreground);

void gfx_blit_rect(const BootInfo *surface, uint32_t *destination,
                   const uint32_t *source, uint32_t x, uint32_t y,
                   uint32_t width, uint32_t height);

void gfx_blit_frame(const BootInfo *surface, const uint32_t *source,
                    uint32_t source_pitch_pixels, uint32_t source_width,
                    uint32_t source_height);

#endif
