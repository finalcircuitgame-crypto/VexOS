/*
 * VexOS DoomGeneric platform backend.
 * Copyright (C) 2026 VexOS contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include "doomgeneric.h"
#include "doomkeys.h"

#define SYS_TICKS 7u
#define SYS_DRAW 9u
#define SYS_KEY_EVENT 10u
#define SYS_FULLSCREEN 11u

static inline uint64_t vex_syscall0(uint64_t n) {
    uint64_t r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n) : "memory", "rcx", "r11");
    return r;
}
static inline uint64_t vex_syscall3(uint64_t n, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "memory", "rcx", "r11");
    return r;
}

void DG_Init(void) {
    (void)vex_syscall0(SYS_FULLSCREEN);
}
void DG_SetWindowTitle(const char *title) { (void)title; }

void DG_DrawFrame(void) {
    if (DG_ScreenBuffer)
        (void)vex_syscall3(SYS_DRAW, (uint64_t)(uintptr_t)DG_ScreenBuffer,
                           DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)vex_syscall0(SYS_TICKS);
}

void DG_SleepMs(uint32_t ms) {
    uint32_t start = DG_GetTicksMs();
    while ((uint32_t)(DG_GetTicksMs() - start) < ms)
        __asm__ volatile("pause");
}

static unsigned char doom_key_from_hid(uint8_t usage) {
    if (usage >= 0x04 && usage <= 0x1d)
        return (unsigned char)('a' + usage - 0x04);
    if (usage >= 0x1e && usage <= 0x26)
        return (unsigned char)('1' + usage - 0x1e);
    if (usage == 0x27) return '0';
    if (usage == 0x28) return KEY_ENTER;
    if (usage == 0x29) return KEY_ESCAPE;
    if (usage == 0x2a) return KEY_BACKSPACE;
    if (usage == 0x2b) return KEY_TAB;
    if (usage == 0x2c) return ' ';
    if (usage == 0x2d) return KEY_MINUS;
    if (usage == 0x2e) return KEY_EQUALS;
    if (usage == 0x4f) return KEY_RIGHTARROW;
    if (usage == 0x50) return KEY_LEFTARROW;
    if (usage == 0x51) return KEY_DOWNARROW;
    if (usage == 0x52) return KEY_UPARROW;
    if (usage == 0xe0 || usage == 0xe4) return KEY_RCTRL;
    if (usage == 0xe2 || usage == 0xe6) return KEY_RALT;
    if (usage == 0xe1 || usage == 0xe5) return KEY_RSHIFT;
    return 0;
}

int DG_GetKey(int *pressed, unsigned char *key) {
    uint64_t event = vex_syscall0(SYS_KEY_EVENT);
    if (!(event & 0x10000u))
        return 0;
    unsigned char mapped = doom_key_from_hid((uint8_t)(event & 0xffu));
    if (!mapped)
        return 0;
    if (pressed) *pressed = (int)((event >> 8) & 1u);
    if (key) *key = mapped;
    return 1;
}
