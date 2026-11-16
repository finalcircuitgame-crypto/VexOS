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

static uint32_t log_append_text(char *buffer, uint32_t offset,
                                const char *value) {
    while (value && *value)
        buffer[offset++] = *value++;
    return offset;
}

static uint32_t log_append_u64(char *buffer, uint32_t offset, uint64_t value) {
    char digits[20];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value && count < sizeof(digits));
    while (count)
        buffer[offset++] = digits[--count];
    return offset;
}

static uint32_t log_append_i32(char *buffer, uint32_t offset, int32_t value) {
    uint32_t magnitude;
    if (value < 0) {
        buffer[offset++] = '-';
        magnitude = (uint32_t)(-(value + 1)) + 1u;
    } else {
        magnitude = (uint32_t)value;
    }
    return log_append_u64(buffer, offset, magnitude);
}

/*
 * Log the actual ticcmd produced by Doom after all keyboard, mouse, and
 * joystick inputs have been combined. This is distinct from KEYSYNC, which
 * only reports the kernel HID snapshot copied into gamekeydown[].
 */
void VEXOS_DoomTraceTiccmd(int maketic, int angleturn,
                           int forwardmove, int sidemove) {
    static int previous_valid;
    static int previous_turn;
    static int previous_forward;
    static int previous_side;

    if (previous_valid && angleturn == previous_turn &&
        forwardmove == previous_forward && sidemove == previous_side)
        return;

    previous_valid = 1;
    previous_turn = angleturn;
    previous_forward = forwardmove;
    previous_side = sidemove;

    char message[160];
    uint32_t length = 0;
    int active = angleturn != 0 || forwardmove != 0 || sidemove != 0;
    length = log_append_text(message, length, "[DOOM][CMD] tick=");
    length = log_append_u64(message, length, vex_syscall0(SYS_TICKS));
    length = log_append_text(message, length, " tic=");
    length = log_append_i32(message, length, maketic);
    length = log_append_text(message, length, active ? " state=ACTIVE" : " state=NEUTRAL");
    length = log_append_text(message, length, " turn=");
    length = log_append_i32(message, length, angleturn);
    length = log_append_text(message, length, " forward=");
    length = log_append_i32(message, length, forwardmove);
    length = log_append_text(message, length, " side=");
    length = log_append_i32(message, length, sidemove);
    message[length++] = '\n';
    (void)vex_syscall3(1u, (uint64_t)(uintptr_t)message, length, 0);
}

static const char *arrow_label_from_usage(uint8_t usage) {
    switch (usage) {
        case 0x4F: return "RIGHT";
        case 0x50: return "LEFT";
        case 0x51: return "DOWN";
        case 0x52: return "UP";
        default: return 0;
    }
}

static void log_arrow_transition(const char *stage, uint8_t usage,
                                 int pressed, const char *source) {
    const char *key = arrow_label_from_usage(usage);
    if (!key)
        return;

    char message[128];
    uint32_t length = 0;
    length = log_append_text(message, length, "[DOOM] tick=");
    length = log_append_u64(message, length, vex_syscall0(SYS_TICKS));
    length = log_append_text(message, length, " stage=");
    length = log_append_text(message, length, stage);
    length = log_append_text(message, length, " key=");
    length = log_append_text(message, length, key);
    length = log_append_text(message, length, " state=");
    length = log_append_text(message, length, pressed ? "DOWN" : "UP");
    length = log_append_text(message, length, " source=");
    length = log_append_text(message, length, source);
    message[length++] = '\n';
    (void)vex_syscall3(1u, (uint64_t)(uintptr_t)message, length, 0);
}

static void log_arrow_ticcmd_state(uint8_t state) {
    static uint8_t previous_state;
    static uint8_t state_valid;
    static const uint8_t bits[4] = {1u, 2u, 4u, 8u};
    static const uint8_t usages[4] = {0x4Fu, 0x50u, 0x51u, 0x52u};

    if (!state_valid) {
        previous_state = state;
        state_valid = 1u;
        return;
    }

    uint8_t changed = (uint8_t)(previous_state ^ state);
    previous_state = state;
    for (uint32_t i = 0; i < 4u; ++i) {
        if (changed & bits[i])
            log_arrow_transition("KEYSYNC", usages[i],
                                 (state & bits[i]) != 0, "kernel-snapshot");
    }
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

#ifdef VEXOS_CI_INPUT_TEST
static uint8_t arrow_sync_state;
static uint8_t arrow_sync_state_valid;

static void trace_arrow_sync_state(uint8_t state) {
    static const uint8_t bits[4] = {1u, 2u, 4u, 8u};
    static const char *const down_messages[4] = {
        "[CI] Doom key state synced RIGHT down\n",
        "[CI] Doom key state synced LEFT down\n",
        "[CI] Doom key state synced DOWN down\n",
        "[CI] Doom key state synced UP down\n"
    };
    static const char *const up_messages[4] = {
        "[CI] Doom key state synced RIGHT released\n",
        "[CI] Doom key state synced LEFT released\n",
        "[CI] Doom key state synced DOWN released\n",
        "[CI] Doom key state synced UP released\n"
    };
    static const uint8_t down_lengths[4] = {
        sizeof("[CI] Doom key state synced RIGHT down\n") - 1u,
        sizeof("[CI] Doom key state synced LEFT down\n") - 1u,
        sizeof("[CI] Doom key state synced DOWN down\n") - 1u,
        sizeof("[CI] Doom key state synced UP down\n") - 1u
    };
    static const uint8_t up_lengths[4] = {
        sizeof("[CI] Doom key state synced RIGHT released\n") - 1u,
        sizeof("[CI] Doom key state synced LEFT released\n") - 1u,
        sizeof("[CI] Doom key state synced DOWN released\n") - 1u,
        sizeof("[CI] Doom key state synced UP released\n") - 1u
    };

    if (!arrow_sync_state_valid) {
        arrow_sync_state = state;
        arrow_sync_state_valid = 1;
        return;
    }

    uint8_t changed = (uint8_t)(arrow_sync_state ^ state);
    arrow_sync_state = state;
    for (uint32_t i = 0; i < 4u; ++i) {
        if (!(changed & bits[i]))
            continue;
        const char *message = (state & bits[i]) ? down_messages[i] : up_messages[i];
        uint64_t length = (state & bits[i]) ? down_lengths[i] : up_lengths[i];
        (void)vex_syscall3(1u, (uint64_t)(uintptr_t)message, length, 0);
    }
}
#else
#define trace_arrow_sync_state(state) ((void)0)
#endif

/*
 * Keep Doom's continuous movement booleans synchronized with the live
 * kernel HID snapshot. Input events remain responsible for menus and other
 * one-shot actions; movement no longer depends on a key-up queue surviving.
 */
void VEXOS_DoomSyncArrowKeys(unsigned int *keys, int key_right, int key_left,
                             int key_down, int key_up) {
    if (!keys)
        return;

    uint8_t state = (uint8_t)(vex_syscall0(SYS_KEY_STATE) & 0x0Fu);
    if (key_right > 0 && key_right < 256)
        keys[key_right] = (state & 1u) ? 1u : 0u;
    if (key_left > 0 && key_left < 256)
        keys[key_left] = (state & 2u) ? 1u : 0u;
    if (key_down > 0 && key_down < 256)
        keys[key_down] = (state & 4u) ? 1u : 0u;
    if (key_up > 0 && key_up < 256)
        keys[key_up] = (state & 8u) ? 1u : 0u;

    log_arrow_ticcmd_state(state);
    trace_arrow_sync_state(state);
}

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

/*
 * On-screen input monitor for manual held-key verification.
 * Green arrows mean the kernel currently sees that key held; slate arrows
 * mean it sees the key released. It is painted into the captured Doom frame
 * so an MP4 recording can show the exact transition without a second window.
 */
static void draw_arrow_debug_overlay(uint32_t *pixels, uint32_t width,
                                     uint32_t height) {
    static const uint8_t arrow_rows[4][7] = {
        {0x08u, 0x1Cu, 0x3Eu, 0x08u, 0x08u, 0x08u, 0x08u}, /* Up */
        {0x08u, 0x08u, 0x08u, 0x08u, 0x3Eu, 0x1Cu, 0x08u}, /* Down */
        {0x08u, 0x18u, 0x38u, 0x7Fu, 0x38u, 0x18u, 0x08u}, /* Left */
        {0x08u, 0x0Cu, 0x0Eu, 0x7Fu, 0x0Eu, 0x0Cu, 0x08u}  /* Right */
    };
    static const uint8_t bits[4] = {8u, 4u, 2u, 1u};
    static const uint32_t positions[4][2] = {
        {8u, 8u}, {24u, 8u}, {8u, 24u}, {24u, 24u}
    };
    const uint32_t panel = 0x00101824u;
    const uint32_t released = 0x00485868u;
    const uint32_t pressed = 0x0028C76Fu;
    const uint32_t white = 0x00FFFFFFu;

    if (!pixels || width < 42u || height < 42u)
        return;

    uint8_t state = (uint8_t)(vex_syscall0(SYS_KEY_STATE) & 0x0Fu);

    /* Opaque backing keeps the indicators readable over any Doom scene. */
    for (uint32_t y = 5u; y < 43u; ++y) {
        for (uint32_t x = 5u; x < 43u; ++x)
            pixels[y * width + x] = panel;
    }

    for (uint32_t key = 0; key < 4u; ++key) {
        uint32_t x0 = positions[key][0];
        uint32_t y0 = positions[key][1];
        uint32_t background = (state & bits[key]) ? pressed : released;

        for (uint32_t y = 0; y < 14u; ++y) {
            for (uint32_t x = 0; x < 14u; ++x)
                pixels[(y0 + y) * width + x0 + x] = background;
        }

        for (uint32_t y = 0; y < 7u; ++y) {
            for (uint32_t x = 0; x < 7u; ++x) {
                if (arrow_rows[key][y] & (uint8_t)(1u << (6u - x)))
                    pixels[(y0 + 3u + y) * width + x0 + 3u + x] = white;
            }
        }
    }
}

void DG_DrawFrame(void) {
    if (DG_ScreenBuffer) {
        draw_arrow_debug_overlay(DG_ScreenBuffer,
                                 DOOMGENERIC_RESX, DOOMGENERIC_RESY);
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
            uint8_t usage = (uint8_t)(i == 0u ? 0x4Fu : i == 1u ? 0x50u : i == 2u ? 0x51u : 0x52u);
            log_arrow_transition("EVENT", usage, (actual & bit) != 0, "state-reconcile");
            trace_arrow_event(usage, (actual & bit) != 0);
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
            log_arrow_transition("EVENT", usage, is_pressed, "event-queue");
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
