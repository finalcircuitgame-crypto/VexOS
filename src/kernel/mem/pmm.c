#include "../include/pmm.h"
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096ULL

static uint8_t *bitmap;
static uint64_t max_pages;
static uint64_t base_paddr;

static uint64_t pmm_irq_save_disable(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void pmm_irq_restore(uint64_t flags) {
    if (flags & (1ull << 9))
        __asm__ volatile("sti" ::: "memory");
}

extern void serial_print(const char *str);

static void debug_print(const char *str) {
    serial_print(str);
}

static void debug_hex(uint64_t val) {
    static const char digits[] = "0123456789ABCDEF";
    char text[17];

    for (int i = 15; i >= 0; --i)
        text[15 - i] = digits[(val >> (i * 4)) & 0xFu];
    text[16] = '\0';
    serial_print(text);
}

void PMM_Init(uint64_t mem_size, void *bitmap_addr) {
    bitmap = NULL;
    max_pages = 0;
    base_paddr = 0;

    if (!bitmap_addr || mem_size < PAGE_SIZE) {
        debug_print("[PMM] Init failed: invalid region\n");
        return;
    }

    bitmap = (uint8_t *)bitmap_addr;
    max_pages = mem_size / PAGE_SIZE;
    base_paddr = (uint64_t)(uintptr_t)bitmap_addr;

    debug_print("[PMM] Init: max_pages=");
    debug_hex(max_pages);
    debug_print("\n");

    uint64_t bitmap_bytes = (max_pages + 7ULL) / 8ULL;
    for (uint64_t i = 0; i < bitmap_bytes; i++) bitmap[i] = 0xFF;
}

void PMM_FreePages(void *addr, uint64_t count) {
    if (!bitmap || !addr || count == 0) return;

    uint64_t value = (uint64_t)(uintptr_t)addr;
    if (value < base_paddr || ((value - base_paddr) & (PAGE_SIZE - 1ULL)))
        return;

    uint64_t irq_flags = pmm_irq_save_disable();
    uint64_t start_page = (value - base_paddr) / PAGE_SIZE;
    if (start_page >= max_pages) {
        pmm_irq_restore(irq_flags);
        return;
    }
    if (count > max_pages - start_page) count = max_pages - start_page;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t page = start_page + i;
        uint8_t mask = (uint8_t)(1u << (page % 8ULL));
        /* Clearing an already-free bit is harmless and remains idempotent. */
        if (bitmap[page / 8ULL] & mask)
            bitmap[page / 8ULL] &= (uint8_t)~mask;
    }
    pmm_irq_restore(irq_flags);

    debug_print("[PMM] Freeing: start=");
    debug_hex(start_page);
    debug_print(" count=");
    debug_hex(count);
    debug_print("\n");
}

void *PMM_AllocatePage(void) {
    if (!bitmap || max_pages == 0) return NULL;

    uint64_t irq_flags = pmm_irq_save_disable();
    void *allocated = NULL;
    for (uint64_t i = 0; i < max_pages; i++) {
        uint8_t mask = (uint8_t)(1u << (i % 8ULL));
        if (!(bitmap[i / 8ULL] & mask)) {
            bitmap[i / 8ULL] |= mask;
            allocated = (void *)(uintptr_t)(base_paddr + i * PAGE_SIZE);
            break;
        }
    }
    pmm_irq_restore(irq_flags);

    if (!allocated)
        debug_print("[PMM] Allocate: FAILED!\n");
    return allocated;
}

void PMM_FreePage(void *addr) {
    if (!bitmap || !addr) return;

    uint64_t value = (uint64_t)(uintptr_t)addr;
    if (value < base_paddr || ((value - base_paddr) & (PAGE_SIZE - 1ULL)))
        return;

    uint64_t page = (value - base_paddr) / PAGE_SIZE;
    if (page >= max_pages) return;

    uint64_t irq_flags = pmm_irq_save_disable();
    uint8_t mask = (uint8_t)(1u << (page % 8ULL));
    if (bitmap[page / 8ULL] & mask)
        bitmap[page / 8ULL] &= (uint8_t)~mask;
    pmm_irq_restore(irq_flags);
}

/* CI and diagnostics helper: count currently free physical pages without
   changing allocator state. The bitmap uses one bits for allocated pages. */
uint64_t PMM_GetFreePageCount(void) {
    if (!bitmap || max_pages == 0) return 0;

    uint64_t irq_flags = pmm_irq_save_disable();
    uint64_t free_pages = 0;
    for (uint64_t page = 0; page < max_pages; ++page) {
        uint8_t mask = (uint8_t)(1u << (page % 8ULL));
        if (!(bitmap[page / 8ULL] & mask))
            ++free_pages;
    }
    pmm_irq_restore(irq_flags);
    return free_pages;
}
