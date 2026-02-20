#include <stdint.h>
#include "../include/bootinfo.h"

// Embedded font symbols from objcopy
extern uint8_t _binary_CGA_F08_start[];
extern uint8_t _binary_CGA_F08_end[];

static BootInfo *g_BootInfo;
static uint32_t g_CursorX = 0;
static uint32_t g_CursorY = 0;

#define CONSOLE_FONT_W 8u
#define CONSOLE_FONT_H 8u
#define CONSOLE_FONT_SCALE 2u
#define CONSOLE_LINE_GAP 2u

static inline uint32_t console_char_w(void) { return CONSOLE_FONT_W * CONSOLE_FONT_SCALE; }
static inline uint32_t console_char_h(void) { return CONSOLE_FONT_H * CONSOLE_FONT_SCALE; }
static inline uint32_t console_line_advance(void) { return console_char_h() + (CONSOLE_LINE_GAP * CONSOLE_FONT_SCALE); }

void scroll_up(void) {
    uint32_t line_height = console_line_advance();
    uint32_t *fb = g_BootInfo->framebuffer;
    uint32_t pitch = g_BootInfo->pitch;
    uint32_t height = g_BootInfo->height;

    // Copy lines up by one line height
    for (uint32_t y = line_height; y < height; y++) {
        for (uint32_t x = 0; x < g_BootInfo->width; x++) {
            fb[(y - line_height) * pitch + x] = fb[y * pitch + x];
        }
    }

    // Clear the bottom line
    uint32_t clear_y_start = height - line_height;
    for (uint32_t y = clear_y_start; y < height; y++) {
        for (uint32_t x = 0; x < g_BootInfo->width; x++) {
            fb[y * pitch + x] = 0xFF0000FFu; // Blue background
        }
    }
}

void ConsoleInit(BootInfo *bootInfo) {
    g_BootInfo = bootInfo;
}

void PutChar(char c, uint32_t color) {
    if (c == '\n') {
        g_CursorX = 0;
        g_CursorY += console_line_advance();
        if (g_CursorY >= g_BootInfo->height) {
            scroll_up();
            g_CursorY -= console_line_advance();
        }
        return;
    }

    uint32_t *fb = g_BootInfo->framebuffer;
    uint8_t *glyph = &(_binary_CGA_F08_start[(uint8_t)c * 8]);

    for (uint32_t gy = 0; gy < CONSOLE_FONT_H; gy++) {
        uint8_t row = glyph[gy];
        for (uint32_t gx = 0; gx < CONSOLE_FONT_W; gx++) {
            if ((row >> (7 - gx)) & 1u) {
                uint32_t px = g_CursorX + gx * CONSOLE_FONT_SCALE;
                uint32_t py = g_CursorY + gy * CONSOLE_FONT_SCALE;
                for (uint32_t sy = 0; sy < CONSOLE_FONT_SCALE; sy++) {
                    for (uint32_t sx = 0; sx < CONSOLE_FONT_SCALE; sx++) {
                        uint32_t x = px + sx;
                        uint32_t y = py + sy;
                        if (x < g_BootInfo->width && y < g_BootInfo->height) {
                            fb[y * g_BootInfo->pitch + x] = color;
                        }
                    }
                }
            }
        }
    }

    g_CursorX += console_char_w();
    if (g_CursorX + console_char_w() > g_BootInfo->width) {
        g_CursorX = 0;
        g_CursorY += console_line_advance();
        if (g_CursorY >= g_BootInfo->height) {
            scroll_up();
            g_CursorY -= console_line_advance();
        }
    }
}

void PrintString(const char *str, uint32_t color) {
    while (*str) {
        PutChar(*str++, color);
    }
}