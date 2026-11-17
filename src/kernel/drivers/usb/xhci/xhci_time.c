#include "xhci_internal.h"
#include <stdint.h>

/*
 * Early xHCI initialization runs before the kernel enables IRQ0, so Timer_Ticks()
 * is not usable here. Use the invariant TSC when its frequency can be obtained
 * from CPUID, with a short PIT channel-2 calibration as the fallback.
 *
 * The PIT fallback changes channel 2's one-shot configuration. The speaker gate
 * and speaker-data bits are restored immediately; VexOS has no PC-speaker driver.
 */

static uint64_t g_tsc_hz;
static uint8_t g_time_initialized;
static const char *g_tsc_source = "uninitialized";

static inline uint64_t xhci_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline void xhci_cpuid(uint32_t leaf, uint32_t subleaf,
                              uint32_t *a, uint32_t *b,
                              uint32_t *c, uint32_t *d) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(leaf), "c"(subleaf));
    if (a) *a = eax;
    if (b) *b = ebx;
    if (c) *c = ecx;
    if (d) *d = edx;
}

static inline void xhci_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t xhci_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static int xhci_valid_tsc_hz(uint64_t hz) {
    return hz >= 100000000ull && hz <= 20000000000ull;
}

static uint64_t xhci_tsc_hz_from_cpuid(void) {
    uint32_t max_leaf, a, b, c, d;
    xhci_cpuid(0, 0, &max_leaf, 0, 0, 0);

    /* CPUID 0x15: TSC frequency = crystal_hz * numerator / denominator. */
    if (max_leaf >= 0x15u) {
        xhci_cpuid(0x15u, 0, &a, &b, &c, &d);
        if (a && b && c) {
            uint64_t hz = ((uint64_t)c * (uint64_t)b) / (uint64_t)a;
            if (xhci_valid_tsc_hz(hz))
                return hz;
        }
    }

    return 0;
}

static uint64_t xhci_tsc_hz_from_pit(void) {
    /* 59,659 PIT input clocks are almost exactly 50 ms at 1,193,182 Hz. */
    const uint16_t count = 59659u;
    const uint32_t max_polls = 2000000u;
    uint8_t speaker = xhci_inb(0x61u);

    /* Gate low and disable speaker data while programming channel 2 mode 0. */
    xhci_outb(0x61u, (uint8_t)(speaker & (uint8_t)~0x03u));
    xhci_outb(0x43u, 0xB0u); /* channel 2, lobyte/hibyte, mode 0, binary */
    xhci_outb(0x42u, (uint8_t)(count & 0xFFu));
    xhci_outb(0x42u, (uint8_t)(count >> 8));

    uint64_t start = xhci_rdtsc();
    xhci_outb(0x61u, (uint8_t)((speaker & (uint8_t)~0x02u) | 0x01u));

    int saw_low = 0;
    int completed = 0;
    for (uint32_t i = 0; i < max_polls; ++i) {
        uint8_t status = xhci_inb(0x61u);
        if (!(status & 0x20u)) {
            saw_low = 1;
        } else if (saw_low) {
            completed = 1;
            break;
        }
        __asm__ volatile("pause");
    }

    uint64_t end = xhci_rdtsc();
    xhci_outb(0x61u, speaker);

    if (!completed || end <= start)
        return 0;

    uint64_t hz = ((end - start) * 1193182ull) / (uint64_t)count;
    return xhci_valid_tsc_hz(hz) ? hz : 0;
}

static uint64_t xhci_tsc_hz_from_cpuid_leaf16(void) {
    uint32_t max_leaf, a, b, c, d;
    xhci_cpuid(0, 0, &max_leaf, 0, 0, 0);
    if (max_leaf < 0x16u)
        return 0;

    xhci_cpuid(0x16u, 0, &a, &b, &c, &d);
    uint64_t hz = (uint64_t)a * 1000000ull;
    return xhci_valid_tsc_hz(hz) ? hz : 0;
}

void xhci_time_init(void) {
    if (g_time_initialized)
        return;

    g_time_initialized = 1;
    g_tsc_hz = xhci_tsc_hz_from_cpuid();
    if (g_tsc_hz) {
        g_tsc_source = "CPUID.15";
        return;
    }

    g_tsc_hz = xhci_tsc_hz_from_pit();
    if (g_tsc_hz) {
        g_tsc_source = "PIT-channel-2";
        return;
    }

    g_tsc_hz = xhci_tsc_hz_from_cpuid_leaf16();
    if (g_tsc_hz) {
        g_tsc_source = "CPUID.16-estimate";
        return;
    }

    /* This fallback bounds waits but is an estimate, not a calibration. */
    g_tsc_source = "1GHz-fallback";
}

/* Returns a TSC deadline. The 1 GHz fallback keeps waits finite on unusual CPUs
 * that expose neither a usable CPUID frequency nor a working PIT channel 2. */
uint64_t xhci_time_deadline_us(uint64_t timeout_us) {
    if (!g_time_initialized)
        xhci_time_init();

    uint64_t hz = g_tsc_hz ? g_tsc_hz : 1000000000ull;
    uint64_t ticks = (hz / 1000000ull) * timeout_us;
    ticks += ((hz % 1000000ull) * timeout_us) / 1000000ull;
    if (!ticks)
        ticks = 1;
    return xhci_rdtsc() + ticks;
}

int xhci_time_expired(uint64_t deadline) {
    return (int64_t)(xhci_rdtsc() - deadline) >= 0;
}

void xhci_time_delay_us(uint64_t duration_us) {
    uint64_t deadline = xhci_time_deadline_us(duration_us);
    while (!xhci_time_expired(deadline))
        __asm__ volatile("pause");
}

uint64_t xhci_time_frequency_hz(void) {
    if (!g_time_initialized)
        xhci_time_init();
    return g_tsc_hz;
}

uint64_t xhci_time_now_ticks(void) {
    return xhci_rdtsc();
}

static void xhci_time_print_u64(uint64_t value) {
    char buf[21];
    uint32_t used = 0;
    if (value == 0) {
        serial_print("0");
        return;
    }
    while (value && used < sizeof(buf)) {
        buf[used++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (used) {
        char digit[2] = {buf[--used], 0};
        serial_print(digit);
    }
}

void xhci_time_log_clock(void) {
    if (!g_time_initialized)
        xhci_time_init();

    serial_print("[xHCI][TIME] source=");
    serial_print(g_tsc_source);
    serial_print(" tsc_hz=");
    xhci_time_print_u64(g_tsc_hz ? g_tsc_hz : 1000000000ull);
    if (!g_tsc_hz)
        serial_print(" (fallback estimate)");
    serial_print("\n");
}

void xhci_time_log_elapsed(const char *stage, uint64_t start_ticks,
                           const char *result) {
    uint64_t hz = g_tsc_hz ? g_tsc_hz : 1000000000ull;
    uint64_t delta = xhci_rdtsc() - start_ticks;
    uint64_t elapsed_us =
        (delta / hz) * 1000000ull +
        ((delta % hz) * 1000000ull) / hz;

    serial_print("[xHCI][TIME] ");
    serial_print(stage ? stage : "stage");
    serial_print(" result=");
    serial_print(result ? result : "unknown");
    serial_print(" elapsed_us=");
    xhci_time_print_u64(elapsed_us);
    serial_print("\n");
}
