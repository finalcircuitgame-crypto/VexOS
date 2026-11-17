#include "bootlog.h"
#include "uefi_bootlog.h"
#include "vfs.h"
#include <stdint.h>

#define BOOTLOG_PATH "/boot.log"
#define BOOTLOG_NVRAM_MAX_ATTEMPTS 12u

#if !defined(__x86_64__) || !defined(__GNUC__)
#error "UEFI Runtime Services calls require the x86-64 GNU ms_abi calling convention"
#endif

typedef uint64_t (__attribute__((ms_abi)) *VexOSSetVariableFn)(
    const uint16_t *variable_name,
    const VexOSBootLogGuid *vendor_guid,
    uint32_t attributes,
    uint64_t data_size,
    const void *data);

static char g_log_buffer[BOOTLOG_MAX_BYTES];
static char g_flush_buffer[BOOTLOG_MAX_BYTES];
static uint8_t g_nvram_record[
    sizeof(VexOSBootLogHeader) + VEXOS_BOOTLOG_MAX_PAYLOAD
] __attribute__((aligned(8)));
static uint32_t g_log_head;
static uint32_t g_log_used;
static uint32_t g_log_generation;
static uint32_t g_nvram_last_generation;
static uint32_t g_nvram_attempts;
static uint64_t g_set_variable_address;
static uint8_t g_storage_checked;
static uint8_t g_storage_ready;
static uint8_t g_dirty;
static uint8_t g_flushing;
static uint8_t g_nvram_busy;

static uint64_t bootlog_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void bootlog_irq_restore(uint64_t flags) {
    if (flags & (1ull << 9))
        __asm__ volatile("sti" ::: "memory");
}

void bootlog_set_runtime(uint64_t set_variable_address) {
    uint64_t flags = bootlog_irq_save();
    g_set_variable_address = set_variable_address;
    bootlog_irq_restore(flags);
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

int bootlog_checkpoint(void) {
    uint64_t flags = bootlog_irq_save();
    if (!g_set_variable_address || !g_log_used || g_nvram_busy ||
        g_nvram_attempts >= BOOTLOG_NVRAM_MAX_ATTEMPTS ||
        (g_nvram_last_generation != 0 &&
         g_nvram_last_generation == g_log_generation)) {
        int already_saved = g_nvram_last_generation != 0 &&
                            g_nvram_last_generation == g_log_generation;
        bootlog_irq_restore(flags);
        return already_saved;
    }

    g_nvram_busy = 1;
    ++g_nvram_attempts;

    uint32_t length = g_log_used;
    if (length > VEXOS_BOOTLOG_MAX_PAYLOAD)
        length = VEXOS_BOOTLOG_MAX_PAYLOAD;
    uint32_t generation = g_log_generation;
    uint32_t start = (g_log_head + BOOTLOG_MAX_BYTES - length) %
                     BOOTLOG_MAX_BYTES;

    VexOSBootLogHeader *header = (VexOSBootLogHeader *)g_nvram_record;
    uint8_t *payload = g_nvram_record + sizeof(*header);
    for (uint32_t i = 0; i < length; ++i)
        payload[i] = (uint8_t)g_log_buffer[(start + i) % BOOTLOG_MAX_BYTES];

    header->magic = VEXOS_BOOTLOG_MAGIC;
    header->version = VEXOS_BOOTLOG_VERSION;
    header->header_size = (uint16_t)sizeof(*header);
    header->payload_size = length;
    header->checksum = vexos_bootlog_checksum(payload, length);

    uint64_t function_address = g_set_variable_address;
    bootlog_irq_restore(flags);

    /* UEFI x86-64 Runtime Services use the Microsoft x64 ABI, not SysV. */
    VexOSSetVariableFn set_variable =
        (VexOSSetVariableFn)(uintptr_t)function_address;
    uint64_t status = set_variable(
        VEXOS_BOOTLOG_VARIABLE_NAME,
        &VEXOS_BOOTLOG_VENDOR_GUID,
        VEXOS_BOOTLOG_VARIABLE_ATTRIBUTES,
        (uint64_t)sizeof(*header) + length,
        g_nvram_record);

    flags = bootlog_irq_save();
    g_nvram_busy = 0;
    if (status == 0)
        g_nvram_last_generation = generation;
    bootlog_irq_restore(flags);

    return status == 0;
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
        serial_print("[BOOTLOG] No writable VexOS volume; UEFI checkpoint remains available if firmware supports it\n");
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
