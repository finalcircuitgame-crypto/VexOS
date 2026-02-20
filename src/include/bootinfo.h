#ifndef BOOTINFO_H
#define BOOTINFO_H

#include <stdint.h>

typedef struct {
    uint64_t Base;
    uint64_t Size;
} MemoryRegion;

typedef struct {
    uint32_t *framebuffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    MemoryRegion LargestFreeRegion;
} BootInfo;

#endif