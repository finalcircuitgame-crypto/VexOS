#ifndef GDT_H
#define GDT_H

#include <stdint.h>

struct GDTDescriptor {
    uint16_t Size;
    uint64_t Offset;
} __attribute__((packed));

struct GDTEntry {
    uint16_t Limit0;
    uint16_t Base0;
    uint8_t Base1;
    uint8_t Access;
    uint8_t Limit1_Flags;
    uint8_t Base2;
} __attribute__((packed));

struct GDT {
    struct GDTEntry Null;
    struct GDTEntry KernelCode;
    struct GDTEntry KernelData;
    struct GDTEntry UserCode;
    struct GDTEntry UserData;
} __attribute__((packed)) __attribute__((aligned(0x1000)));

void LoadGDT(struct GDTDescriptor* gdtDescriptor);

#endif
