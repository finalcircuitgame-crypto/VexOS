#include <stdint.h>
#include "doomgeneric.h"
#include "doomkeys.h"
#include "d_event.h"
#include "../libc/vexfs.h"

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
    uint32_t arrow_index = 4u;
    static const uint8_t bits[4] = {1u, 2u, 4u, 8u};
    static const char *const state_messages[4][2] = {
        {"[CI] Doom syscall arrow RIGHT state down\n", "[CI] Doom syscall arrow RIGHT state released\n"},
        {"[CI] Doom syscall arrow LEFT state down\n", "[CI] Doom syscall arrow LEFT state released\n"},
        {"[CI] Doom syscall arrow DOWN state down\n", "[CI] Doom syscall arrow DOWN state released\n"},
        {"[CI] Doom syscall arrow UP state down\n", "[CI] Doom syscall arrow UP state released\n"}
    };
    static const uint8_t state_lengths[4][2] = {
        {sizeof("[CI] Doom syscall arrow RIGHT state down\n") - 1u, sizeof("[CI] Doom syscall arrow RIGHT state released\n") - 1u},
        {sizeof("[CI] Doom syscall arrow LEFT state down\n") - 1u, sizeof("[CI] Doom syscall arrow LEFT state released\n") - 1u},
        {sizeof("[CI] Doom syscall arrow DOWN state down\n") - 1u, sizeof("[CI] Doom syscall arrow DOWN state released\n") - 1u},
        {sizeof("[CI] Doom syscall arrow UP state down\n") - 1u, sizeof("[CI] Doom syscall arrow UP state released\n") - 1u}
    };
#define TRACE_ARROW_CASE(code, label, index) \
    case code: \
        arrow_index = index; \
        if (pressed) { \
            message = "[CI] Doom backend received " label " down\n"; \
            length = sizeof("[CI] Doom backend received " label " down\n") - 1u; \
        } else { \
            message = "[CI] Doom backend received " label " released\n"; \
            length = sizeof("[CI] Doom backend received " label " released\n") - 1u; \
        } \
        break
    switch (usage) {
        TRACE_ARROW_CASE(0x4F, "RIGHT", 0u);
        TRACE_ARROW_CASE(0x50, "LEFT", 1u);
        TRACE_ARROW_CASE(0x51, "DOWN", 2u);
        TRACE_ARROW_CASE(0x52, "UP", 3u);
        default: break;
    }
#undef TRACE_ARROW_CASE

    if (message)
        (void)vex_syscall3(1u, (uint64_t)(uintptr_t)message, length, 0);

    if (arrow_index < 4u) {
        uint32_t live_state = (uint32_t)(vex_syscall0(SYS_KEY_STATE) & 0x0Fu);
        uint32_t expected = pressed ? bits[arrow_index] : 0u;
        const char *state_message;
        uint64_t state_length;
        if ((live_state & bits[arrow_index]) == expected) {
            uint32_t state_index = pressed ? 0u : 1u;
            state_message = state_messages[arrow_index][state_index];
            state_length = state_lengths[arrow_index][state_index];
        } else {
            state_message = "[CI] Doom syscall arrow state mismatch\n";
            state_length = sizeof("[CI] Doom syscall arrow state mismatch\n") - 1u;
        }
        (void)vex_syscall3(1u, (uint64_t)(uintptr_t)state_message,
                           state_length, 0);
    }
}
#else
#define trace_arrow_event(usage, pressed) ((void)0)
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

#ifdef VEXOS_CI_INPUT_TEST
static void ci_test_vfs_syscalls(void) {
    static const char path[] = "/r3ci/data.txt";
    static const char parent[] = "/r3ci";
    static const char payload[] = "Ring3 VFS syscall regression";
    char readback[sizeof(payload)];
    vexfs_stat_t info;
    vexfs_dir_t directory = {-1};
    vexfs_dir_t root = {-1};
    vexfs_dirent_t entry;
    int fd = -1;
    int type = -1;

    (void)vexfs_unlink(path);
    if (vexfs_create(path) != 0)
        goto fail;
    if (vexfs_write_file(path, payload, sizeof(payload)) != 0)
        goto fail;
    if (vexfs_stat(path, &info) != 0 ||
        info.type != VEXFS_NODE_FILE || info.size != sizeof(payload))
        goto fail;

    fd = vexfs_open_read(path);
    if (fd < 3)
        goto fail;
    if (vexfs_unlink(path) == 0 ||
        vexfs_write_file(path, payload, sizeof(payload)) == 0)
        goto fail;
    int bytes = vexfs_read_file(fd, readback, sizeof(readback));
    if (bytes != (int)sizeof(payload))
        goto fail;
    for (uint32_t i = 0; i < sizeof(payload); ++i) {
        if (readback[i] != payload[i])
            goto fail;
    }
    if (vexfs_seek_file(fd, 0, 0) != 0 ||
        vexfs_close_file(fd) != 0)
        goto fail;
    fd = -1;

    if (vexfs_open_dir(&directory, parent) < 3)
        goto fail;
    uint32_t entry_size = 0;
    if (vexfs_readdir_sized(&directory, entry.name, 2u,
                            &entry_size) != -1)
        goto fail;
    if (vexfs_readdir_sized(&directory, (char *)(uintptr_t)1u,
                            sizeof(entry.name), &entry_size) != -1)
        goto fail;
    type = vexfs_readdir(&directory, &entry);
    if (type != (int)VEXFS_NODE_FILE ||
        entry.size != sizeof(payload) ||
        entry.name[0] != 'd' || entry.name[1] != 'a' ||
        entry.name[2] != 't' || entry.name[3] != 'a' ||
        entry.name[4] != '.' || entry.name[5] != 't' ||
        entry.name[6] != 'x' || entry.name[7] != 't' ||
        entry.name[8] != '\0' ||
        vexfs_readdir(&directory, &entry) != 0 ||
        vexfs_close_dir(&directory) != 0)
        goto fail;

    if (vexfs_open_dir(&root, "/") < 3)
        goto fail;
    int found_parent = 0;
    while ((type = vexfs_readdir(&root, &entry)) > 0) {
        if (type == (int)VEXFS_NODE_DIRECTORY &&
            entry.name[0] == 'r' && entry.name[1] == '3' &&
            entry.name[2] == 'c' && entry.name[3] == 'i' &&
            entry.name[4] == '\0')
            found_parent = 1;
    }
    if (type < 0 || !found_parent || vexfs_close_dir(&root) != 0)
        goto fail;

    if (vexfs_unlink(path) != 0 ||
        vexfs_stat(path, &info) == 0 ||
        vexfs_stat(parent, &info) == 0 ||
        vexfs_write_file(path, (const void *)(uintptr_t)1u, 4u) == 0)
        goto fail;

    static const char passed[] =
        "[CI] RING3 VFS mutation/stat/directory syscalls passed\n";
    (void)vexfs_syscall3(1u, (uint64_t)(uintptr_t)passed,
                         sizeof(passed) - 1u, 0);
    return;

fail:
    if (fd >= 3)
        (void)vexfs_close_file(fd);
    if (directory.fd >= 3)
        (void)vexfs_close_dir(&directory);
    if (root.fd >= 3)
        (void)vexfs_close_dir(&root);
    (void)vexfs_unlink(path);
    static const char failed[] =
        "[CI] RING3 VFS mutation/stat/directory syscalls FAILED\n";
    (void)vexfs_syscall3(1u, (uint64_t)(uintptr_t)failed,
                         sizeof(failed) - 1u, 0);
}
#endiff

void DG_Init(void) {
    mouse_buttons = 0;
#ifdef VEXOS_CI_INPUT_TEST
    ci_test_vfs_syscalls();
#endif
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
            uint8_t usage = (uint8_t)(i == 0u ? 0x4Fu : i == 1u ? 0x50u : i == 2u ? 0x51u : 0x52u);
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
