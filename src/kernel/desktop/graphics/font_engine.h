#ifndef VEXOS_FONT_ENGINE_H
#define VEXOS_FONT_ENGINE_H

#include <stdint.h>

/* Font family IDs intentionally match gfx_font_family_t. */
typedef struct {
    int32_t advance_x;
    int32_t x_offset;
    int32_t y_offset;
    uint32_t width;
    uint32_t height;
} FontEngineGlyphMetrics;

/* Initialize after Heap_Init(). Font bytes are embedded in the kernel image. */
int FontEngine_Init(void);
int FontEngine_IsReady(void);

/* Pixel height is the font em height, not a bitmap scale factor. */
void FontEngine_GetLineMetrics(uint8_t family, uint8_t bold,
                               uint16_t pixel_height,
                               int32_t *ascent, int32_t *descent,
                               int32_t *line_gap);

int32_t FontEngine_GetKerning(uint8_t family, uint8_t bold,
                              uint16_t pixel_height,
                              uint32_t left_codepoint,
                              uint32_t right_codepoint);

/*
 * Rasterize/cache one glyph and blend it into a 0x00RRGGBB target.
 * pen_x is the glyph origin; baseline_y is the baseline in framebuffer space.
 * The clip rectangle is screen-space and half-open. Returns the horizontal
 * advance in pixels, or zero when the engine is unavailable.
 */
int32_t FontEngine_DrawGlyph(uint8_t family, uint8_t bold,
                             uint16_t pixel_height, uint32_t codepoint,
                             uint32_t *target, uint32_t pitch,
                             uint32_t screen_width, uint32_t screen_height,
                             int32_t pen_x, int32_t baseline_y,
                             uint32_t clip_x, uint32_t clip_y,
                             uint32_t clip_width, uint32_t clip_height,
                             uint32_t foreground,
                             FontEngineGlyphMetrics *metrics);

/* Runtime regression probe used by the QEMU CI boot test. */
int FontEngine_SelfTest(void);

#endif
