#include "../include/pmm.h"
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096ULL

static uint8_t *bitmap;
static uint64_t max_pages;
static uint64_t base_paddr;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void debug_print(const char *str) {
    while (str && *str) outb(0x3F8, (uint8_t)*str++);
}

static void debug_hex(uint64_t val) {
    for (int i = 15; i >= 0; i--) {
        char c = (char)((val >> (i * 4)) & 0xF);
        if (c < 10) c += '0'; else c += 'A' - 10;
        char s[2] = {c, 0};
        debug_print(s);
    }
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

    uint64_t start_page = (value - base_paddr) / PAGE_SIZE;
    if (start_page >= max_pages) return;
    if (count > max_pages - start_page) count = max_pages - start_page;

    debug_print("[PMM] Freeing: start=");
    debug_hex(start_page);
    debug_print(" count=");
    debug_hex(count);
    debug_print("\n");

    for (uint64_t i = 0; i < count; i++) {
        uint64_t page = start_page + i;
        bitmap[page / 8ULL] &= (uint8_t)~(1u << (page % 8ULL));
    }
}

void *PMM_AllocatePage(void) {
    if (!bitmap || max_pages == 0) return NULL;

    for (uint64_t i = 0; i < max_pages; i++) {
        uint8_t mask = (uint8_t)(1u << (i % 8ULL));
        if (!(bitmap[i / 8ULL] & mask)) {
            bitmap[i / 8ULL] |= mask;
            return (void *)(uintptr_t)(base_paddr + i * PAGE_SIZE);
        }
    }
    debug_print("[PMM] Allocate: FAILED!\n");
    return NULL;
}

void PMM_FreePage(void *addr) {
    if (!bitmap || !addr) return;

    uint64_t value = (uint64_t)(uintptr_t)addr;
    if (value < base_paddr || ((value - base_paddr) & (PAGE_SIZE - 1ULL)))
        return;

    uint64_t page = (value - base_paddr) / PAGE_SIZE;
    if (page < max_pages) {
        bitmap[page / 8ULL] &= (uint8_t)~(1u << (page % 8ULL));
    }
}
