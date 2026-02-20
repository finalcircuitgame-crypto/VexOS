#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include "bootinfo.h"

// We'll need the UEFI memory map eventually, but for now, let's define a simple bitmap-based PMM
void PMM_Init(uint64_t mem_size, void* bitmap_addr);
void* PMM_AllocatePage();
void PMM_FreePage(void* addr);

#endif
