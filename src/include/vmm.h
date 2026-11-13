#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

typedef uint64_t page_table_entry;

typedef struct {
    page_table_entry entries[512];
} page_table;

int VMM_Init(void);
int VMM_MapPage(void *virtual_addr, void *physical_addr, uint64_t flags);
int VMM_UnmapPage(void *virtual_addr, int free_physical);
int VMM_SetPageWrite(void *virtual_addr, int writable);
int VMM_IsUserRange(const void *address, uint64_t length, int write_access);
void VMM_Activate(void);
page_table *VMM_GetKernelPML4(void);

#define PAGE_PRESENT (1ULL << 0)
#define PAGE_WRITE   (1ULL << 1)
#define PAGE_USER    (1ULL << 2)

#endif
