#ifndef VEXOS_GRAPHICS_H
#define VEXOS_GRAPHICS_H

#include <stdint.h>
#include "../../include/bootinfo.h"

void gfx_init(BootInfo *boot_info, uint32_t *backbuffer, const uint8_t *font);
void gfx_set_backbuffer(uint32_t *backbuffer);
void gfx_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void gfx_draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);
void gfx_blend_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color, uint8_t alpha);
void gfx_blend_round_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t radius, uint32_t color, uint8_t alpha);
void gfx_draw_glyph(uint32_t x, uint32_t y, uint8_t character, uint32_t foreground);
void gfx_draw_string(uint32_t x, uint32_t y, const char *text, uint32_t foreground);

#endif
