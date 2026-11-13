#include "../../include/vmm.h"
#include "../../include/pmm.h"
#include <stddef.h>
#include <stdint.h>

#define USER_ADDRESS_LIMIT 0x0000800000000000ULL
#define PHYSICAL_ADDRESS_MASK 0x000FFFFFFFFFF000ULL

static page_table *kernel_pml4;

static int is_canonical(uint64_t address) {
    uint64_t upper = address >> 48;
    return (address & (1ULL << 47)) ? upper == 0xFFFFULL : upper == 0;
}

static void clear_page(void *addr) {
    if (!addr) return;
    uint64_t *ptr = (uint64_t *)addr;
    for (uint32_t i = 0; i < 512; i++) ptr[i] = 0;
}

static page_table *get_or_create_table(page_table *parent, uint64_t index,
                                       uint64_t table_flags) {
    uint64_t entry = parent->entries[index];
    if (entry & PAGE_PRESENT) {
        if (table_flags & PAGE_USER) parent->entries[index] |= PAGE_USER;
        return (page_table *)(uintptr_t)(entry & PHYSICAL_ADDRESS_MASK);
    }

    void *new_table = PMM_AllocatePage();
    if (!new_table) return NULL;
    clear_page(new_table);
    parent->entries[index] = ((uint64_t)(uintptr_t)new_table & PHYSICAL_ADDRESS_MASK) |
                             table_flags;
    return (page_table *)new_table;
}

int VMM_Init(void) {
    kernel_pml4 = (page_table *)PMM_AllocatePage();
    if (!kernel_pml4) return 0;
    clear_page(kernel_pml4);
    return 1;
}

int VMM_MapPage(void *virtual_addr, void *physical_addr, uint64_t flags) {
    uint64_t v = (uint64_t)(uintptr_t)virtual_addr;
    uint64_t p = (uint64_t)(uintptr_t)physical_addr;

    if (!kernel_pml4 || (v & (PAGE_SIZE - 1u)) != 0 ||
        (p & (PAGE_SIZE - 1u)) != 0 || (p & ~PHYSICAL_ADDRESS_MASK) != 0 ||
        !is_canonical(v)) {
        return 0;
    }
    if ((flags & PAGE_USER) && v >= USER_ADDRESS_LIMIT) return 0;

    uint64_t table_flags = PAGE_PRESENT | PAGE_WRITE;
    if (flags & PAGE_USER) table_flags |= PAGE_USER;

    uint64_t pml4_idx = (v >> 39) & 0x1FF;
    uint64_t pdp_idx  = (v >> 30) & 0x1FF;
    uint64_t pd_idx   = (v >> 21) & 0x1FF;
    uint64_t pt_idx   = (v >> 12) & 0x1FF;

    page_table *pdp = get_or_create_table(kernel_pml4, pml4_idx, table_flags);
    if (!pdp) return 0;

    page_table *pd = get_or_create_table(pdp, pdp_idx, table_flags);
    if (!pd) return 0;

    page_table *pt = get_or_create_table(pd, pd_idx, table_flags);
    if (!pt) return 0;

    uint64_t old_entry = pt->entries[pt_idx];
    uint64_t new_entry = (p & PHYSICAL_ADDRESS_MASK) |
                         (flags & 0xFFFULL) | PAGE_PRESENT;

    if (old_entry & PAGE_PRESENT) {
        // Never replace a live mapping with a different physical page, and
        // never make an existing supervisor page user-accessible.
        if ((old_entry & PHYSICAL_ADDRESS_MASK) !=
            (new_entry & PHYSICAL_ADDRESS_MASK)) return 0;
        if ((flags & PAGE_USER) && !(old_entry & PAGE_USER)) return 0;
        pt->entries[pt_idx] = old_entry | (new_entry & 0xFFFULL);
    } else {
        pt->entries[pt_idx] = new_entry;
    }

    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    return 1;
}

int VMM_UnmapPage(void *virtual_addr, int free_physical) {
    uint64_t v = (uint64_t)(uintptr_t)virtual_addr;
    if (!kernel_pml4 || (v & (PAGE_SIZE - 1u)) != 0 || !is_canonical(v))
        return 0;

    uint64_t i4 = (v >> 39) & 0x1FF;
    uint64_t i3 = (v >> 30) & 0x1FF;
    uint64_t i2 = (v >> 21) & 0x1FF;
    uint64_t i1 = (v >> 12) & 0x1FF;

    uint64_t e4 = kernel_pml4->entries[i4];
    if (!(e4 & PAGE_PRESENT)) return 0;
    page_table *pdpt = (page_table *)(uintptr_t)(e4 & PHYSICAL_ADDRESS_MASK);

    uint64_t e3 = pdpt->entries[i3];
    if (!(e3 & PAGE_PRESENT)) return 0;
    page_table *pd = (page_table *)(uintptr_t)(e3 & PHYSICAL_ADDRESS_MASK);

    uint64_t e2 = pd->entries[i2];
    if (!(e2 & PAGE_PRESENT)) return 0;
    page_table *pt = (page_table *)(uintptr_t)(e2 & PHYSICAL_ADDRESS_MASK);

    uint64_t entry = pt->entries[i1];
    if (!(entry & PAGE_PRESENT)) return 0;
    if (free_physical && !(entry & PAGE_USER)) return 0;

    pt->entries[i1] = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    if (free_physical) {
        PMM_FreePage((void *)(uintptr_t)(entry & PHYSICAL_ADDRESS_MASK));
    }
    return 1;
}

int VMM_SetPageWrite(void *virtual_addr, int writable) {
    uint64_t v = (uint64_t)(uintptr_t)virtual_addr;
    if (!kernel_pml4 || (v & (PAGE_SIZE - 1u)) != 0 ||
        v >= USER_ADDRESS_LIMIT || !is_canonical(v))
        return 0;

    uint64_t i4 = (v >> 39) & 0x1FF;
    uint64_t i3 = (v >> 30) & 0x1FF;
    uint64_t i2 = (v >> 21) & 0x1FF;
    uint64_t i1 = (v >> 12) & 0x1FF;

    uint64_t e4 = kernel_pml4->entries[i4];
    if ((e4 & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER))
        return 0;
    page_table *pdpt = (page_table *)(uintptr_t)(e4 & PHYSICAL_ADDRESS_MASK);

    uint64_t e3 = pdpt->entries[i3];
    if ((e3 & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER))
        return 0;
    page_table *pd = (page_table *)(uintptr_t)(e3 & PHYSICAL_ADDRESS_MASK);

    uint64_t e2 = pd->entries[i2];
    if ((e2 & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER))
        return 0;
    page_table *pt = (page_table *)(uintptr_t)(e2 & PHYSICAL_ADDRESS_MASK);

    uint64_t *entry = &pt->entries[i1];
    if ((*entry & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER))
        return 0;

    if (writable)
        *entry |= PAGE_WRITE;
    else
        *entry &= ~PAGE_WRITE;

    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    return 1;
}

int VMM_IsUserRange(const void *address, uint64_t length, int write_access) {
    uint64_t start = (uint64_t)(uintptr_t)address;
    if (length == 0) return 1;
    if (!kernel_pml4 || start == 0 || start >= USER_ADDRESS_LIMIT ||
        length > USER_ADDRESS_LIMIT - start) return 0;

    uint64_t end = start + length - 1;
    uint64_t page = start & ~(uint64_t)(PAGE_SIZE - 1u);
    uint64_t last_page = end & ~(uint64_t)(PAGE_SIZE - 1u);

    for (;;) {
        uint64_t i4 = (page >> 39) & 0x1FF;
        uint64_t i3 = (page >> 30) & 0x1FF;
        uint64_t i2 = (page >> 21) & 0x1FF;
        uint64_t i1 = (page >> 12) & 0x1FF;

        uint64_t e4 = kernel_pml4->entries[i4];
        if ((e4 & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER))
            return 0;
        page_table *pdpt = (page_table *)(uintptr_t)(e4 & PHYSICAL_ADDRESS_MASK);

        uint64_t e3 = pdpt->entries[i3];
        if ((e3 & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER))
            return 0;
        page_table *pd = (page_table *)(uintptr_t)(e3 & PHYSICAL_ADDRESS_MASK);

        uint64_t e2 = pd->entries[i2];
        if ((e2 & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER))
            return 0;
        page_table *pt = (page_table *)(uintptr_t)(e2 & PHYSICAL_ADDRESS_MASK);

        uint64_t e1 = pt->entries[i1];
        if ((e1 & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER))
            return 0;
        if (write_access && !(e1 & PAGE_WRITE)) return 0;

        if (page == last_page) break;
        page += PAGE_SIZE;
    }
    return 1;
}

void VMM_Activate(void) {
    if (!kernel_pml4) return;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_pml4) : "memory");
}

page_table *VMM_GetKernelPML4(void) {
    return kernel_pml4;
}
