#include "bootlog.h"
#include "vfs.h"
#include <stdint.h>

#define BOOTLOG_PATH "/boot.log"
static char g_log_buffer[BOOTLOG_MAX_BYTES];
static char g_flush_buffer[BOOTLOG_MAX_BYTES];
static uint32_t g_log_head;
static uint32_t g_log_used;
static uint32_t g_log_generation;
static uint8_t g_storage_checked;
static uint8_t g_storage_ready;
static uint8_t g_dirty;
static uint8_t g_flushing;

static uint64_t bootlog_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void bootlog_irq_restore(uint64_t flags) {
    if (flags & (1ull << 9))
        __asm__ volatile("sti" ::: "memory");
}

void bootlog_append(const char *text) {
    if (!text)
        return;

    uint64_t flags = bootlog_irq_save();
    uint32_t appended = 0;

    while (*text) {
        g_log_buffer[g_log_head] = *text++;
        g_log_head = (g_log_head + 1u) % BOOTLOG_MAX_BYTES;
        if (g_log_used < BOOTLOG_MAX_BYTES)
            ++g_log_used;
        ++appended;
    }

    if (appended) {
        ++g_log_generation;
        g_dirty = 1;
    }

    bootlog_irq_restore(flags);
}

int bootlog_prepare(void) {
    uint64_t flags = bootlog_irq_save();
    if (g_storage_checked) {
        int ready = g_storage_ready != 0;
        bootlog_irq_restore(flags);
        return ready;
    }
    /* Set this before mounting: mount diagnostics also pass through serial_print. */
    g_storage_checked = 1;
    bootlog_irq_restore(flags);

    if (!vfs_init() || !vfs_is_writable()) {
        extern void serial_print(const char *str);
        serial_print("[BOOTLOG] No writable VexOS volume; retaining log in RAM\n");
        return 0;
    }

    flags = bootlog_irq_save();
    g_storage_ready = 1;
    bootlog_irq_restore(flags);

    extern void serial_print(const char *str);
    serial_print("[BOOTLOG] Writable storage ready; log destination is /boot.log\n");
    return 1;
}

int bootlog_flush(void) {
    if (!g_storage_checked && !bootlog_prepare())
        return 0;

    uint64_t flags = bootlog_irq_save();
    if (!g_storage_ready || !g_dirty || g_flushing) {
        int result = g_storage_ready && !g_dirty;
        bootlog_irq_restore(flags);
        return result;
    }

    g_flushing = 1;
    uint32_t length = g_log_used;
    uint32_t generation = g_log_generation;
    uint32_t start = (g_log_head + BOOTLOG_MAX_BYTES - g_log_used) %
                     BOOTLOG_MAX_BYTES;

    for (uint32_t i = 0; i < length; ++i)
        g_flush_buffer[i] =
            g_log_buffer[(start + i) % BOOTLOG_MAX_BYTES];

    bootlog_irq_restore(flags);

    /* VFS only writes to already-recognized writable SFS/SFS2 volumes. */
    int success = vfs_write(BOOTLOG_PATH, g_flush_buffer, length);

    flags = bootlog_irq_save();
    g_flushing = 0;
    if (success && generation == g_log_generation)
        g_dirty = 0;
    bootlog_irq_restore(flags);

    return success;
}
