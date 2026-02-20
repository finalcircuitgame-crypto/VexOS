#include "../include/pmm.h"
#include <stddef.h>

#define PAGE_SIZE 4096

static uint8_t* bitmap;
static uint64_t max_pages;
static uint64_t base_paddr;

// Serial Port output (minimal version for debugging)
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static void debug_print(const char *str) {
    while (*str) outb(0x3F8, *str++);
}
static void debug_hex(uint64_t val) {
    for (int i = 15; i >= 0; i--) {
        char c = (val >> (i * 4)) & 0xF;
        if (c < 10) c += '0'; else c += 'A' - 10;
        char s[2] = {c, 0};
        debug_print(s);
    }
}

void PMM_Init(uint64_t mem_size, void* bitmap_addr) {
    bitmap = (uint8_t*)bitmap_addr;
    max_pages = mem_size / PAGE_SIZE;
    base_paddr = (uint64_t)bitmap_addr;

    debug_print("[PMM] Init: max_pages=");
    debug_hex(max_pages);
    debug_print("\n");

    // Initialize bitmap (mark all as used/reserved initially)
    for (uint64_t i = 0; i < (max_pages + 7) / 8; i++) {
        bitmap[i] = 0xFF;
    }
}

// Mark a range of pages as free
void PMM_FreePages(void* addr, uint64_t count) {
    uint64_t start_page = ((uint64_t)addr - base_paddr) / PAGE_SIZE;
    debug_print("[PMM] Freeing: start=");
    debug_hex(start_page);
    debug_print(" count=");
    debug_hex(count);
    debug_print("\n");
    for (uint64_t i = 0; i < count; i++) {
        uint64_t page = start_page + i;
        if (page >= max_pages) break;
        bitmap[page / 8] &= ~(1 << (page % 8));
    }
}

void* PMM_AllocatePage() {
    for (uint64_t i = 0; i < max_pages; i++) {
        if (!(bitmap[i / 8] & (1 << (i % 8)))) {
            bitmap[i / 8] |= (1 << (i % 8)); // Mark as used
            return (void*)(base_paddr + (i * PAGE_SIZE));
        }
    }
    debug_print("[PMM] Allocate: FAILED!\n");
    return NULL; // Out of memory
}

void PMM_FreePage(void* addr) {
    uint64_t page = ((uint64_t)addr - base_paddr) / PAGE_SIZE;
    if (page < max_pages) {
        bitmap[page / 8] &= ~(1 << (page % 8));
    }
}