#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

typedef uint64_t page_table_entry;
typedef struct {
    page_table_entry entries[512];
} page_table;

typedef struct AddressSpace AddressSpace;

int VMM_Init(void);
int VMM_MapPage(void *virtual_addr, void *physical_addr, uint64_t flags);
int VMM_UnmapPage(void *virtual_addr, int free_physical);
int VMM_SetPageWrite(void *virtual_addr, int writable);
int VMM_IsUserRange(const void *address, uint64_t length, int write_access);
void VMM_Activate(void);
page_table *VMM_GetKernelPML4(void);
AddressSpace *VMM_GetActiveAddressSpace(void);

/* Each address space receives a private copy of the current kernel page-table
   hierarchy. Kernel leaf mappings are inherited; user leaf frames are owned
   and reclaimed by AddressSpaceDestroy. */
AddressSpace *VMM_AddressSpaceCreate(void);
void VMM_AddressSpaceDestroy(AddressSpace *space);
int VMM_AddressSpaceMapPage(AddressSpace *space, void *virtual_addr,
                            void *physical_addr, uint64_t flags);
int VMM_AddressSpaceUnmapPage(AddressSpace *space, void *virtual_addr,
                              int free_physical);
int VMM_AddressSpaceSetPageWrite(AddressSpace *space, void *virtual_addr,
                                 int writable);
int VMM_AddressSpaceIsUserRange(AddressSpace *space, const void *address,
                                uint64_t length, int write_access);
void VMM_ActivateAddressSpace(AddressSpace *space);

#define PAGE_PRESENT (1ULL << 0)
#define PAGE_WRITE   (1ULL << 1)
#define PAGE_USER    (1ULL << 2)

#endif
