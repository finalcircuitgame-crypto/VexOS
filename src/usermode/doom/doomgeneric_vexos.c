#include <stdint.h>
#include "doomgeneric.h"
#include "doomkeys.h"
#include "d_event.h"

#define SYS_EXIT       0u
#define SYS_TICKS      7u
#define SYS_DRAW       9u
#define SYS_KEY_EVENT 10u
#define SYS_APP_READY 11u
#define SYS_MOUSE_EVENT 12u
#define SYS_SHOULD_CLOSE 13u
#define SYS_KEY_STATE 14u

static uint32_t mouse_buttons;
static uint8_t arrow_state;
static uint8_t arrow_state_valid;

static inline uint64_t vex_syscall0(uint64_t n) {
    uint64_t r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n)
                     : "memory", "rcx", "r11");
    return r;
}

static inline uint64_t vex_syscall3(uint64_t n, uint64_t a, uint64_t b,
                                    uint64_t c) {
    uint64_t r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "memory", "rcx", "r11");
    return r;
}

#ifdef VEXOS_CI_INPUT_TEST
static void trace_arrow_event(uint8_t usage, int pressed) {
    const char *message = 0;
    uint64_t length = 0;
#define TRACE_ARROW_CASE(code, label) \
    case code: \
        if (pressed) { \
            message = "[CI] Doom backend received " label " down\n"; \
            length = sizeof("[CI] Doom backend received " label " down\n") - 1u; \
        } else { \
            message = "[CI] Doom backend received " label " released\n"; \
            length = sizeof("[CI] Doom backend received " label " released\n") - 1u; \
        } \
        break
    switch (usage) {
        TRACE_ARROW_CASE(0x4F, "RIGHT");
        TRACE_ARROW_CASE(0x50, "LEFT");
        TRACE_ARROW_CASE(0x51, "DOWN");
        TRACE_ARROW_CASE(0x52, "UP");
        default: break;
    }
#undef TRACE_ARROW_CASE
    if (message)
        (void)vex_syscall3(1u, (uint64_t)(uintptr_t)message, length, 0);
}
#else
#define trace_arrow_event(usage, pressed) ((void)0)
#endif

static int arrow_index_from_usage(uint8_t usage) {
    switch (usage) {
        case 0x4F: return 0; /* Right */
        case 0x50: return 1; /* Left */
        case 0x51: return 2; /* Down */
        case 0x52: return 3; /* Up */
        default: return -1;
    }
}

static unsigned char arrow_key_from_index(uint32_t index) {
    static const unsigned char keys[4] = {
        KEY_RIGHTARROW, KEY_LEFTARROW, KEY_DOWNARROW, KEY_UPARROW
    };
    return index < 4u ? keys[index] : 0;
}

void DG_Init(void) {
    mouse_buttons = 0;
    (void)vex_syscall0(SYS_APP_READY);
    arrow_state = (uint8_t)(vex_syscall0(SYS_KEY_STATE) & 0x0Fu);
    arrow_state_valid = 1;
}

void DG_SetWindowTitle(const char *title) {
    (void)title;
}

void DG_DrawFrame(void) {
    if (DG_ScreenBuffer) {
        (void)vex_syscall3(SYS_DRAW,
                           (uint64_t)(uintptr_t)DG_ScreenBuffer,
                           DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    }
}

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)vex_syscall0(SYS_TICKS);
}

void DG_SleepMs(uint32_t ms) {
    uint32_t start = DG_GetTicksMs();
    while ((uint32_t)(DG_GetTicksMs() - start) < ms)
        __asm__ volatile("pause");
}

int DG_ShouldClose(void) {
    return vex_syscall0(SYS_SHOULD_CLOSE) != 0;
}

__attribute__((noreturn)) void DG_Exit(void) {
    (void)vex_syscall0(SYS_EXIT);
    for (;;)
        __asm__ volatile("pause");
}

static void poll_mouse(void) {
    /* Consume host pointer motion so it cannot accumulate, but do not feed
       absolute USB-tablet cursor deltas into Doom's relative mouse-look. */
    uint64_t v = vex_syscall0(SYS_MOUSE_EVENT);
    uint32_t buttons = (uint32_t)((v >> 32) & 7u);
    if (buttons != mouse_buttons) {
        event_t ev;
        ev.type = ev_mouse;
        ev.data1 = (int)buttons;
        ev.data2 = 0;
        ev.data3 = 0;
        ev.data4 = 0;
        D_PostEvent(&ev);
        mouse_buttons = buttons;
    }
}

static unsigned char doom_key_from_hid(uint8_t usage) {
    if (usage >= 0x04 && usage <= 0x1D)
        return (unsigned char)('a' + usage - 0x04);
    if (usage >= 0x1E && usage <= 0x26)
        return (unsigned char)('1' + usage - 0x1E);

    if (usage == 0x27) return '0';
    if (usage == 0x28) return KEY_ENTER;
    if (usage == 0x29) return KEY_ESCAPE;
    if (usage == 0x2A) return KEY_BACKSPACE;
    if (usage == 0x2B) return KEY_TAB;
    if (usage == 0x2C) return KEY_USE;
    if (usage == 0x2D) return KEY_MINUS;
    if (usage == 0x2E) return KEY_EQUALS;
    if (usage >= 0x3A && usage <= 0x45)
        return (unsigned char)(KEY_F1 + usage - 0x3A);

    switch (usage) {
        case 0x4F: return KEY_RIGHTARROW;
        case 0x50: return KEY_LEFTARROW;
        case 0x51: return KEY_DOWNARROW;
        case 0x52: return KEY_UPARROW;
        case 0xE0:
        case 0xE4: return KEY_FIRE;
        case 0xE2:
        case 0xE6: return KEY_RALT;
        case 0xE1:
        case 0xE5: return KEY_RSHIFT;
        default: return 0;
    }
}

/*
 * A full kernel HID queue can drop a transition. The kernel still records
 * the current physical arrow-key state, so compare it after draining queued
 * events and synthesize a missing arrow down/up transition when necessary.
 */
static int reconcile_arrow_state(int *pressed, unsigned char *key) {
    uint8_t actual = (uint8_t)(vex_syscall0(SYS_KEY_STATE) & 0x0Fu);
    if (!arrow_state_valid) {
        arrow_state = actual;
        arrow_state_valid = 1;
        return 0;
    }

    uint8_t changed = (uint8_t)(actual ^ arrow_state);
    if (!changed)
        return 0;

    for (uint32_t i = 0; i < 4u; ++i) {
        uint8_t bit = (uint8_t)(1u << i);
        if (changed & bit) {
            arrow_state = (uint8_t)((arrow_state & (uint8_t)~bit) |
                                    (actual & bit));
            if (pressed)
                *pressed = (actual & bit) != 0;
            if (key)
                *key = arrow_key_from_index(i);
            trace_arrow_event((uint8_t)(i == 0u ? 0x4Fu : i == 1u ? 0x50u : i == 2u ? 0x51u : 0x52u),
                              (actual & bit) != 0);
            return 1;
        }
    }
    return 0;
}

int DG_GetKey(int *pressed, unsigned char *key) {
    poll_mouse();

    /*
     * Keep polling past HID usages Doom does not map. Returning 0 for an
     * unsupported modifier/device key would otherwise stop DoomGeneric's
     * event loop before a later arrow-key release was consumed.
     */
    for (uint32_t attempt = 0; attempt < 128u; ++attempt) {
        uint64_t ev = vex_syscall0(SYS_KEY_EVENT);
        if (!(ev & 0x10000u))
            break;

        uint8_t usage = (uint8_t)(ev & 0xFFu);
        unsigned char mapped = doom_key_from_hid(usage);
        if (!mapped)
            continue;

        int is_pressed = (int)((ev >> 8) & 1u);
        int arrow_index = arrow_index_from_usage(usage);
        if (arrow_index >= 0) {
            uint8_t bit = (uint8_t)(1u << (uint32_t)arrow_index);
            if (is_pressed)
                arrow_state |= bit;
            else
                arrow_state &= (uint8_t)~bit;
            arrow_state_valid = 1;
            trace_arrow_event(usage, is_pressed);
        }

        if (pressed)
            *pressed = is_pressed;
        if (key)
            *key = mapped;
        return 1;
    }

    return reconcile_arrow_state(pressed, key);
}
