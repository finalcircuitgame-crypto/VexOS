#include "../include/vmm.h"
#include "../include/pmm.h"
#include <stddef.h>

static page_table* kernel_pml4;

// Helper to clear a page
static void clear_page(void* addr) {
    uint64_t* ptr = (uint64_t*)addr;
    for (int i = 0; i < 512; i++) {
        ptr[i] = 0;
    }
}

void VMM_Init() {
    kernel_pml4 = (page_table*)PMM_AllocatePage();
    clear_page(kernel_pml4);
}

void VMM_MapPage(void* virtual_addr, void* physical_addr, uint64_t flags) {
    uint64_t v = (uint64_t)virtual_addr;
    uint64_t p = (uint64_t)physical_addr;

    uint64_t pml4_idx = (v >> 39) & 0x1FF;
    uint64_t pdp_idx  = (v >> 30) & 0x1FF;
    uint64_t pd_idx   = (v >> 21) & 0x1FF;
    uint64_t pt_idx   = (v >> 12) & 0x1FF;

    // PML4 -> PDP
    if (!(kernel_pml4->entries[pml4_idx] & PAGE_PRESENT)) {
        void* new_table = PMM_AllocatePage();
        clear_page(new_table);
        kernel_pml4->entries[pml4_idx] = (uint64_t)new_table | PAGE_PRESENT | PAGE_WRITE;
    }
    page_table* pdp = (page_table*)(kernel_pml4->entries[pml4_idx] & ~0xFFFULL);

    // PDP -> PD
    if (!(pdp->entries[pdp_idx] & PAGE_PRESENT)) {
        void* new_table = PMM_AllocatePage();
        clear_page(new_table);
        pdp->entries[pdp_idx] = (uint64_t)new_table | PAGE_PRESENT | PAGE_WRITE;
    }
    page_table* pd = (page_table*)(pdp->entries[pdp_idx] & ~0xFFFULL);

    // PD -> PT
    if (!(pd->entries[pd_idx] & PAGE_PRESENT)) {
        void* new_table = PMM_AllocatePage();
        clear_page(new_table);
        pd->entries[pd_idx] = (uint64_t)new_table | PAGE_PRESENT | PAGE_WRITE;
    }
    page_table* pt = (page_table*)(pd->entries[pd_idx] & ~0xFFFULL);

    // PT entry
    pt->entries[pt_idx] = p | flags | PAGE_PRESENT;
}

void VMM_Activate() {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_pml4));
}

page_table* VMM_GetKernelPML4() {
    return kernel_pml4;
}
