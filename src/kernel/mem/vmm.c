#include "../../include/vmm.h"
#include "../../include/pmm.h"
#include "../../include/heap.h"
#include <stddef.h>
#include <stdint.h>

#define USER_ADDRESS_LIMIT 0x0000800000000000ULL
#define PHYSICAL_ADDRESS_MASK 0x000FFFFFFFFFF000ULL
#define PAGE_LARGE (1ULL << 7)

struct AddressSpace {
    page_table *pml4;
    uint64_t id;
};

static page_table *kernel_pml4;
static page_table *active_pml4;
static AddressSpace *active_address_space;
static uint64_t next_address_space_id = 1;

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
        /* A large-page leaf is not a pointer to a lower-level table. */
        if (entry & PAGE_LARGE) return NULL;
        return (page_table *)(uintptr_t)(entry & PHYSICAL_ADDRESS_MASK);
    }

    void *new_table = PMM_AllocatePage();
    if (!new_table) return NULL;
    clear_page(new_table);
    parent->entries[index] = ((uint64_t)(uintptr_t)new_table & PHYSICAL_ADDRESS_MASK) |
                             table_flags;
    return (page_table *)new_table;
}

/* Every address space owns its page-table pages. Leaf physical pages remain
   shared unless PAGE_USER marks them as process-owned memory. */
static void destroy_table_tree(page_table *table, uint32_t level,
                               int free_user_leaf_pages) {
    if (!table) return;
    if (level > 1) {
        for (uint32_t i = 0; i < 512; ++i) {
            uint64_t entry = table->entries[i];
            if (!(entry & PAGE_PRESENT) || (entry & PAGE_LARGE)) continue;
            page_table *child = (page_table *)(uintptr_t)
                (entry & PHYSICAL_ADDRESS_MASK);
            destroy_table_tree(child, level - 1, free_user_leaf_pages);
        }
    } else if (free_user_leaf_pages) {
        for (uint32_t i = 0; i < 512; ++i) {
            uint64_t entry = table->entries[i];
            if ((entry & (PAGE_PRESENT | PAGE_USER)) ==
                (PAGE_PRESENT | PAGE_USER)) {
                PMM_FreePage((void *)(uintptr_t)
                    (entry & PHYSICAL_ADDRESS_MASK));
            }
        }
    }
    PMM_FreePage(table);
}

static page_table *clone_table_tree(const page_table *source, uint32_t level) {
    if (!source) return NULL;
    page_table *copy = (page_table *)PMM_AllocatePage();
    if (!copy) return NULL;
    for (uint32_t i = 0; i < 512; ++i) copy->entries[i] = source->entries[i];

    if (level > 1) {
        for (uint32_t i = 0; i < 512; ++i) {
            uint64_t entry = source->entries[i];
            if (!(entry & PAGE_PRESENT) || (entry & PAGE_LARGE)) continue;
            const page_table *source_child = (const page_table *)(uintptr_t)
                (entry & PHYSICAL_ADDRESS_MASK);
            page_table *child_copy = clone_table_tree(source_child, level - 1);
            if (!child_copy) {
                /* Only recurse through children already replaced in this copy. */
                for (uint32_t j = 0; j < i; ++j) {
                    uint64_t prior = copy->entries[j];
                    if ((prior & PAGE_PRESENT) && !(prior & PAGE_LARGE)) {
                        destroy_table_tree((page_table *)(uintptr_t)
                            (prior & PHYSICAL_ADDRESS_MASK), level - 1, 0);
                    }
                }
                PMM_FreePage(copy);
                return NULL;
            }
            /* Preserve every non-address bit, including NX and software flags. */
            copy->entries[i] = ((uint64_t)(uintptr_t)child_copy &
                                PHYSICAL_ADDRESS_MASK) |
                               (entry & ~PHYSICAL_ADDRESS_MASK);
        }
    }
    return copy;
}

static int map_page_in_root(page_table *root, void *virtual_addr,
                            void *physical_addr, uint64_t flags) {
    uint64_t v = (uint64_t)(uintptr_t)virtual_addr;
    uint64_t p = (uint64_t)(uintptr_t)physical_addr;

    if (!root || (v & (PAGE_SIZE - 1u)) != 0 ||
        (p & (PAGE_SIZE - 1u)) != 0 || (p & ~PHYSICAL_ADDRESS_MASK) != 0 ||
        !is_canonical(v)) return 0;
    if ((flags & PAGE_USER) && v >= USER_ADDRESS_LIMIT) return 0;

    uint64_t table_flags = PAGE_PRESENT | PAGE_WRITE;
    if (flags & PAGE_USER) table_flags |= PAGE_USER;

    uint64_t pml4_idx = (v >> 39) & 0x1FF;
    uint64_t pdp_idx  = (v >> 30) & 0x1FF;
    uint64_t pd_idx   = (v >> 21) & 0x1FF;
    uint64_t pt_idx   = (v >> 12) & 0x1FF;

    int created_pdp = !(root->entries[pml4_idx] & PAGE_PRESENT);
    page_table *pdp = get_or_create_table(root, pml4_idx, table_flags);
    if (!pdp) return 0;

    int created_pd = !(pdp->entries[pdp_idx] & PAGE_PRESENT);
    page_table *pd = get_or_create_table(pdp, pdp_idx, table_flags);
    if (!pd) {
        if (created_pdp) {
            root->entries[pml4_idx] = 0;
            PMM_FreePage(pdp);
        }
        return 0;
    }

    page_table *pt = get_or_create_table(pd, pd_idx, table_flags);
    if (!pt) {
        if (created_pd) {
            pdp->entries[pdp_idx] = 0;
            PMM_FreePage(pd);
        }
        if (created_pdp) {
            root->entries[pml4_idx] = 0;
            PMM_FreePage(pdp);
        }
        return 0;
    }

    uint64_t old_entry = pt->entries[pt_idx];
    uint64_t new_entry = (p & PHYSICAL_ADDRESS_MASK) |
                         (flags & 0xFFFULL) | PAGE_PRESENT;
    if (old_entry & PAGE_PRESENT) {
        if ((old_entry & PHYSICAL_ADDRESS_MASK) !=
            (new_entry & PHYSICAL_ADDRESS_MASK)) return 0;
        if ((flags & PAGE_USER) && !(old_entry & PAGE_USER)) return 0;
        pt->entries[pt_idx] = old_entry | (new_entry & 0xFFFULL);
    } else {
        pt->entries[pt_idx] = new_entry;
    }

    /* Promote ancestor permissions only after the leaf is committed. This
       keeps a failed map from leaving partially modified user permissions. */
    if (flags & PAGE_USER) {
        root->entries[pml4_idx] |= PAGE_USER;
        pdp->entries[pdp_idx] |= PAGE_USER;
        pd->entries[pd_idx] |= PAGE_USER;
    }

    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    return 1;
}

static int page_table_is_empty(const page_table *table) {
    if (!table) return 1;
    for (uint32_t i = 0; i < 512u; ++i) {
        if (table->entries[i] & PAGE_PRESENT)
            return 0;
    }
    return 1;
}

static int unmap_page_in_root(page_table *root, void *virtual_addr,
                              int free_physical) {
    uint64_t v = (uint64_t)(uintptr_t)virtual_addr;
    if (!root || (v & (PAGE_SIZE - 1u)) != 0 || !is_canonical(v)) return 0;

    uint64_t i4 = (v >> 39) & 0x1FF, i3 = (v >> 30) & 0x1FF;
    uint64_t i2 = (v >> 21) & 0x1FF, i1 = (v >> 12) & 0x1FF;
    uint64_t e4 = root->entries[i4];
    if (!(e4 & PAGE_PRESENT) || (e4 & PAGE_LARGE)) return 0;
    page_table *pdpt = (page_table *)(uintptr_t)(e4 & PHYSICAL_ADDRESS_MASK);
    uint64_t e3 = pdpt->entries[i3];
    if (!(e3 & PAGE_PRESENT) || (e3 & PAGE_LARGE)) return 0;
    page_table *pd = (page_table *)(uintptr_t)(e3 & PHYSICAL_ADDRESS_MASK);
    uint64_t e2 = pd->entries[i2];
    if (!(e2 & PAGE_PRESENT) || (e2 & PAGE_LARGE)) return 0;
    page_table *pt = (page_table *)(uintptr_t)(e2 & PHYSICAL_ADDRESS_MASK);
    uint64_t entry = pt->entries[i1];
    if (!(entry & PAGE_PRESENT)) return 0;
    if (free_physical && !(entry & PAGE_USER)) return 0;

    pt->entries[i1] = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    if (free_physical)
        PMM_FreePage((void *)(uintptr_t)(entry & PHYSICAL_ADDRESS_MASK));

    /* Release now-empty page-table branches. Inherited kernel branches remain
       populated and therefore cannot be pruned by this path. */
    if (page_table_is_empty(pt)) {
        pd->entries[i2] = 0;
        PMM_FreePage(pt);
        if (page_table_is_empty(pd)) {
            pdpt->entries[i3] = 0;
            PMM_FreePage(pd);
            if (page_table_is_empty(pdpt)) {
                root->entries[i4] = 0;
                PMM_FreePage(pdpt);
            }
        }
    }
    return 1;
}

static int set_page_write_in_root(page_table *root, void *virtual_addr,
                                  int writable) {
    uint64_t v = (uint64_t)(uintptr_t)virtual_addr;
    if (!root || (v & (PAGE_SIZE - 1u)) != 0 ||
        v >= USER_ADDRESS_LIMIT || !is_canonical(v)) return 0;
    uint64_t i4 = (v >> 39) & 0x1FF, i3 = (v >> 30) & 0x1FF;
    uint64_t i2 = (v >> 21) & 0x1FF, i1 = (v >> 12) & 0x1FF;
    uint64_t e4 = root->entries[i4];
    if ((e4 & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER)) return 0;
    page_table *pdpt = (page_table *)(uintptr_t)(e4 & PHYSICAL_ADDRESS_MASK);
    uint64_t e3 = pdpt->entries[i3];
    if ((e3 & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER)) return 0;
    page_table *pd = (page_table *)(uintptr_t)(e3 & PHYSICAL_ADDRESS_MASK);
    uint64_t e2 = pd->entries[i2];
    if ((e2 & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER)) return 0;
    page_table *pt = (page_table *)(uintptr_t)(e2 & PHYSICAL_ADDRESS_MASK);
    uint64_t *entry = &pt->entries[i1];
    if ((*entry & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER)) return 0;
    if (writable) *entry |= PAGE_WRITE;
    else *entry &= ~PAGE_WRITE;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    return 1;
}

static int is_user_range_in_root(page_table *root, const void *address,
                                 uint64_t length, int write_access) {
    uint64_t start = (uint64_t)(uintptr_t)address;
    if (length == 0) return 1;
    if (!root || start == 0 || start >= USER_ADDRESS_LIMIT ||
        length > USER_ADDRESS_LIMIT - start) return 0;
    uint64_t end = start + length - 1;
    uint64_t page = start & ~(uint64_t)(PAGE_SIZE - 1u);
    uint64_t last_page = end & ~(uint64_t)(PAGE_SIZE - 1u);
    for (;;) {
        uint64_t i4 = (page >> 39) & 0x1FF, i3 = (page >> 30) & 0x1FF;
        uint64_t i2 = (page >> 21) & 0x1FF, i1 = (page >> 12) & 0x1FF;
        uint64_t e4 = root->entries[i4];
        if ((e4 & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER)) return 0;
        page_table *pdpt = (page_table *)(uintptr_t)(e4 & PHYSICAL_ADDRESS_MASK);
        uint64_t e3 = pdpt->entries[i3];
        if ((e3 & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER)) return 0;
        page_table *pd = (page_table *)(uintptr_t)(e3 & PHYSICAL_ADDRESS_MASK);
        uint64_t e2 = pd->entries[i2];
        if ((e2 & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER)) return 0;
        page_table *pt = (page_table *)(uintptr_t)(e2 & PHYSICAL_ADDRESS_MASK);
        uint64_t e1 = pt->entries[i1];
        if ((e1 & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER)) return 0;
        if (write_access && !(e1 & PAGE_WRITE)) return 0;
        if (page == last_page) break;
        page += PAGE_SIZE;
    }
    return 1;
}

int VMM_Init(void) {
    kernel_pml4 = (page_table *)PMM_AllocatePage();
    if (!kernel_pml4) return 0;
    clear_page(kernel_pml4);
    active_pml4 = kernel_pml4;
    active_address_space = NULL;
    return 1;
}

/* PAGE_USER leaf frames are owned by their address space and released once
   during teardown. Do not let two user virtual pages claim the same frame. */
static int address_space_has_user_frame_alias(const AddressSpace *space,
                                               uint64_t physical,
                                               uint64_t target_va) {
    if (!space || !space->pml4) return 0;

    uint64_t target_page = target_va & ~(uint64_t)(PAGE_SIZE - 1u);
    for (uint32_t i4 = 0; i4 < 256u; ++i4) {
        uint64_t e4 = space->pml4->entries[i4];
        if (!(e4 & PAGE_PRESENT) || (e4 & PAGE_LARGE)) continue;
        const page_table *pdpt = (const page_table *)(uintptr_t)
            (e4 & PHYSICAL_ADDRESS_MASK);

        for (uint32_t i3 = 0; i3 < 512u; ++i3) {
            uint64_t e3 = pdpt->entries[i3];
            if (!(e3 & PAGE_PRESENT)) continue;
            if (e3 & PAGE_LARGE) {
                if (e3 & PAGE_USER) return 1;
                continue;
            }
            const page_table *pd = (const page_table *)(uintptr_t)
                (e3 & PHYSICAL_ADDRESS_MASK);

            for (uint32_t i2 = 0; i2 < 512u; ++i2) {
                uint64_t e2 = pd->entries[i2];
                if (!(e2 & PAGE_PRESENT)) continue;
                if (e2 & PAGE_LARGE) {
                    if (e2 & PAGE_USER) return 1;
                    continue;
                }
                const page_table *pt = (const page_table *)(uintptr_t)
                    (e2 & PHYSICAL_ADDRESS_MASK);

                for (uint32_t i1 = 0; i1 < 512u; ++i1) {
                    uint64_t e1 = pt->entries[i1];
                    if ((e1 & (PAGE_PRESENT | PAGE_USER)) !=
                        (PAGE_PRESENT | PAGE_USER) ||
                        (e1 & PHYSICAL_ADDRESS_MASK) != physical)
                        continue;

                    uint64_t mapped_va =
                        ((uint64_t)i4 << 39) |
                        ((uint64_t)i3 << 30) |
                        ((uint64_t)i2 << 21) |
                        ((uint64_t)i1 << 12);
                    if (mapped_va != target_page)
                        return 1;
                }
            }
        }
    }
    return 0;
}

int VMM_MapPage(void *virtual_addr, void *physical_addr, uint64_t flags) {
    if (active_address_space)
        return VMM_AddressSpaceMapPage(active_address_space, virtual_addr,
                                       physical_addr, flags);
    return map_page_in_root(active_pml4, virtual_addr, physical_addr, flags);
}
int VMM_UnmapPage(void *virtual_addr, int free_physical) {
    return unmap_page_in_root(active_pml4, virtual_addr, free_physical);
}
int VMM_SetPageWrite(void *virtual_addr, int writable) {
    return set_page_write_in_root(active_pml4, virtual_addr, writable);
}
int VMM_IsUserRange(const void *address, uint64_t length, int write_access) {
    return is_user_range_in_root(active_pml4, address, length, write_access);
}

AddressSpace *VMM_AddressSpaceCreate(void) {
    if (!kernel_pml4) return NULL;
    AddressSpace *space = (AddressSpace *)kmalloc(sizeof(AddressSpace));
    if (!space) return NULL;
    page_table *root = clone_table_tree(kernel_pml4, 4);
    if (!root) {
        kfree(space);
        return NULL;
    }
    space->pml4 = root;
    space->id = next_address_space_id++;
    if (next_address_space_id == 0) next_address_space_id = 1;
    return space;
}

void VMM_AddressSpaceDestroy(AddressSpace *space) {
    if (!space || !space->pml4 || space == active_address_space) return;
    destroy_table_tree(space->pml4, 4, 1);
    space->pml4 = NULL;
    kfree(space);
}

int VMM_AddressSpaceMapPage(AddressSpace *space, void *va, void *pa,
                            uint64_t flags) {
    if (!space || !space->pml4)
        return 0;
    if ((flags & PAGE_USER) &&
        address_space_has_user_frame_alias(
            space, (uint64_t)(uintptr_t)pa,
            (uint64_t)(uintptr_t)va))
        return 0;
    return map_page_in_root(space->pml4, va, pa, flags);
}
int VMM_AddressSpaceUnmapPage(AddressSpace *space, void *va, int free_physical) {
    return space ? unmap_page_in_root(space->pml4, va, free_physical) : 0;
}
int VMM_AddressSpaceSetPageWrite(AddressSpace *space, void *va, int writable) {
    return space ? set_page_write_in_root(space->pml4, va, writable) : 0;
}
int VMM_AddressSpaceIsUserRange(AddressSpace *space, const void *va,
                                uint64_t length, int write_access) {
    return space ? is_user_range_in_root(space->pml4, va, length, write_access) : 0;
}

void VMM_ActivateAddressSpace(AddressSpace *space) {
    if (!space || !space->pml4) return;
    active_pml4 = space->pml4;
    active_address_space = space;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(space->pml4) : "memory");
}

void VMM_Activate(void) {
    if (!kernel_pml4) return;
    active_pml4 = kernel_pml4;
    active_address_space = NULL;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_pml4) : "memory");
}

page_table *VMM_GetKernelPML4(void) { return kernel_pml4; }
AddressSpace *VMM_GetActiveAddressSpace(void) { return active_address_space; }
