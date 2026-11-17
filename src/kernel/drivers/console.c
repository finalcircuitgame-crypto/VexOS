#include <stdint.h>
#include "../include/bootinfo.h"
#include "../desktop/graphics/font_engine.h"

/* The original CGA bitmap is retained only for early-boot fallback. */
extern uint8_t _binary_CGA_F08_start[];
extern uint8_t _binary_CGA_F08_end[];

static BootInfo *g_BootInfo;
static uint32_t g_CursorX = 0;
static uint32_t g_CursorY = 0;
static uint32_t g_PreviousCodepoint;

#define CONSOLE_FONT_PIXEL_HEIGHT 16u
#define CONSOLE_FONT_CELL_WIDTH 16u
#define CONSOLE_FONT_LINE_GAP 2u

static uint32_t console_line_advance(void) {
    if (FontEngine_IsReady()) {
        int32_t ascent = 0, descent = 0, line_gap = 0;
        FontEngine_GetLineMetrics(0, 0, CONSOLE_FONT_PIXEL_HEIGHT,
                                  &ascent, &descent, &line_gap);
        int32_t advance = ascent - descent + line_gap +
                          (int32_t)CONSOLE_FONT_LINE_GAP;
        if (advance > 0)
            return (uint32_t)advance;
    }
    return CONSOLE_FONT_CELL_WIDTH + CONSOLE_FONT_LINE_GAP;
}

static uint32_t console_fallback_line_advance(void) {
    return CONSOLE_FONT_CELL_WIDTH + CONSOLE_FONT_LINE_GAP;
}

void scroll_up(void) {
    if (!g_BootInfo || !g_BootInfo->framebuffer)
        return;
    uint32_t line_height = console_line_advance();
    if (!line_height || line_height >= g_BootInfo->height)
        line_height = g_BootInfo->height;

    uint32_t *fb = g_BootInfo->framebuffer;
    uint32_t pitch = g_BootInfo->pitch;
    uint32_t height = g_BootInfo->height;

    for (uint32_t y = line_height; y < height; y++) {
        for (uint32_t x = 0; x < g_BootInfo->width; x++)
            fb[(y - line_height) * pitch + x] = fb[y * pitch + x];
    }

    uint32_t clear_y_start = height - line_height;
    for (uint32_t y = clear_y_start; y < height; y++) {
        for (uint32_t x = 0; x < g_BootInfo->width; x++)
            fb[y * pitch + x] = 0xFF0000FFu;
    }
}

void ConsoleInit(BootInfo *bootInfo) {
    g_BootInfo = bootInfo;
    g_CursorX = 0;
    g_CursorY = 0;
    g_PreviousCodepoint = 0;
}

static uint32_t console_blend(uint32_t background, uint32_t foreground,
                              uint8_t alpha) {
    uint32_t inverse = 255u - alpha;
    uint32_t red = ((((background >> 16) & 255u) * inverse) +
                    (((foreground >> 16) & 255u) * alpha)) / 255u;
    uint32_t green = ((((background >> 8) & 255u) * inverse) +
                      (((foreground >> 8) & 255u) * alpha)) / 255u;
    uint32_t blue = (((background & 255u) * inverse) +
                     ((foreground & 255u) * alpha)) / 255u;
    return (red << 16) | (green << 8) | blue;
}

static void console_put_fallback(uint32_t codepoint, uint32_t color) {
    uint8_t character = codepoint <= 255u ? (uint8_t)codepoint : (uint8_t)'?';
    uint8_t *glyph = &(_binary_CGA_F08_start[character * 8u]);
    uint32_t *fb = g_BootInfo->framebuffer;

    for (uint32_t gy = 0; gy < 8u; ++gy) {
        for (uint32_t gx = 0; gx < 8u; ++gx) {
            if (!(glyph[gy] & (uint8_t)(1u << (7u - gx))))
                continue;
            uint32_t px = g_CursorX + gx * 2u;
            uint32_t py = g_CursorY + gy * 2u;
            for (uint32_t sy = 0; sy < 2u; ++sy) {
                for (uint32_t sx = 0; sx < 2u; ++sx) {
                    uint32_t x = px + sx;
                    uint32_t y = py + sy;
                    if (x < g_BootInfo->width && y < g_BootInfo->height) {
                        uint32_t *pixel = &fb[y * g_BootInfo->pitch + x];
                        *pixel = console_blend(*pixel, color, 255u);
                    }
                }
            }
        }
    }
}

static void console_advance_line(void) {
    g_CursorX = 0;
    g_PreviousCodepoint = 0;
    g_CursorY += console_line_advance();
    if (g_CursorY + console_line_advance() > g_BootInfo->height) {
        scroll_up();
        uint32_t line = console_line_advance();
        g_CursorY = g_BootInfo->height > line
            ? g_BootInfo->height - line : 0;
    }
}

static void console_put_codepoint(uint32_t codepoint, uint32_t color) {
    if (!g_BootInfo || !g_BootInfo->framebuffer)
        return;
    if (codepoint == '\n') {
        console_advance_line();
        return;
    }
    if (codepoint == '\r') {
        g_CursorX = 0;
        g_PreviousCodepoint = 0;
        return;
    }
    if (codepoint == '\t') {
        g_CursorX += CONSOLE_FONT_CELL_WIDTH * 4u;
        g_PreviousCodepoint = 0;
        return;
    }

    if (FontEngine_IsReady()) {
        int32_t ascent = 0, descent = 0, line_gap = 0;
        FontEngine_GetLineMetrics(0, 0, CONSOLE_FONT_PIXEL_HEIGHT,
                                  &ascent, &descent, &line_gap);
        int32_t pen_x = (int32_t)g_CursorX;
        if (g_PreviousCodepoint)
            pen_x += FontEngine_GetKerning(
                0, 0, CONSOLE_FONT_PIXEL_HEIGHT,
                g_PreviousCodepoint, codepoint);
        if (pen_x < 0)
            pen_x = 0;

        FontEngineGlyphMetrics metrics;
        int32_t advance = FontEngine_DrawGlyph(
            0, 0, CONSOLE_FONT_PIXEL_HEIGHT, codepoint,
            g_BootInfo->framebuffer, g_BootInfo->pitch,
            g_BootInfo->width, g_BootInfo->height,
            pen_x, (int32_t)(g_CursorY + (uint32_t)ascent),
            0, 0, g_BootInfo->width, g_BootInfo->height,
            color, &metrics);
        if (advance <= 0)
            advance = (int32_t)(CONSOLE_FONT_PIXEL_HEIGHT / 2u);
        g_CursorX = (uint32_t)(pen_x + advance);
        g_PreviousCodepoint = codepoint;
    } else {
        console_put_fallback(codepoint, color);
        g_CursorX += CONSOLE_FONT_CELL_WIDTH;
    }

    if (g_CursorX + CONSOLE_FONT_CELL_WIDTH > g_BootInfo->width)
        console_advance_line();
}

void PutChar(char c, uint32_t color) {
    console_put_codepoint((uint8_t)c, color);
}

static uint32_t console_decode_utf8(const char **text) {
    const uint8_t *s = (const uint8_t *)*text;
    uint32_t cp;
    if (!s || !s[0])
        return 0;
    if (s[0] < 0x80u) {
        *text += 1;
        return s[0];
    }
    if (s[0] >= 0xC2u && s[0] <= 0xDFu) {
        if (!s[1] || (s[1] & 0xC0u) != 0x80u) {
            *text += 1;
            return 0xFFFDu;
        }
        cp = ((uint32_t)(s[0] & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
        *text += 2;
        return cp;
    }
    if (s[0] >= 0xE0u && s[0] <= 0xEFu) {
        if (!s[1] || !s[2] || (s[1] & 0xC0u) != 0x80u ||
            (s[2] & 0xC0u) != 0x80u ||
            (s[0] == 0xE0u && s[1] < 0xA0u) ||
            (s[0] == 0xEDu && s[1] >= 0xA0u)) {
            *text += 1;
            return 0xFFFDu;
        }
        cp = ((uint32_t)(s[0] & 0x0Fu) << 12) |
             ((uint32_t)(s[1] & 0x3Fu) << 6) | (uint32_t)(s[2] & 0x3Fu);
        *text += 3;
        return cp;
    }
    if (s[0] >= 0xF0u && s[0] <= 0xF4u) {
        if (!s[1] || !s[2] || !s[3] ||
            (s[1] & 0xC0u) != 0x80u ||
            (s[2] & 0xC0u) != 0x80u ||
            (s[3] & 0xC0u) != 0x80u ||
            (s[0] == 0xF0u && s[1] < 0x90u) ||
            (s[0] == 0xF4u && s[1] >= 0x90u)) {
            *text += 1;
            return 0xFFFDu;
        }
        cp = ((uint32_t)(s[0] & 0x07u) << 18) |
             ((uint32_t)(s[1] & 0x3Fu) << 12) |
             ((uint32_t)(s[2] & 0x3Fu) << 6) | (uint32_t)(s[3] & 0x3Fu);
        *text += 4;
        return cp;
    }
    *text += 1;
    return 0xFFFDu;
}

void PrintString(const char *str, uint32_t color) {
    while (str && *str)
        console_put_codepoint(console_decode_utf8(&str), color);
}
