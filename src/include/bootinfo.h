#ifndef BOOTINFO_H
#define BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_MAX_RUNTIME_REGIONS 32u

typedef struct {
    uint64_t Base;
    uint64_t Size;
} MemoryRegion;

typedef struct {
    uint64_t Base;
    uint64_t Pages;
} BootRuntimeRegion;

typedef struct {
    uint32_t *framebuffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    MemoryRegion LargestFreeRegion;

    /* Physical UEFI Runtime Services entry and identity-mapped memory ranges. */
    uint64_t RuntimeSetVariable;
    uint32_t RuntimeRegionCount;
    uint32_t RuntimeReserved;
    BootRuntimeRegion RuntimeRegions[BOOTINFO_MAX_RUNTIME_REGIONS];
} BootInfo;

#endif
