#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

typedef uint64_t page_table_entry;

typedef struct {
    page_table_entry entries[512];
} page_table;

void VMM_Init();
void VMM_MapPage(void* virtual_addr, void* physical_addr, uint64_t flags);
void VMM_Activate();
page_table* VMM_GetKernelPML4();

#define PAGE_PRESENT (1ULL << 0)
#define PAGE_WRITE   (1ULL << 1)
#define PAGE_USER    (1ULL << 2)

#endif
