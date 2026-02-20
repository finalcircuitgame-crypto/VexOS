#include "gdt.h"

struct GDT DefaultGDT = {
    {0, 0, 0, 0, 0, 0},             // Null
    {0, 0, 0, 0x9a, 0xa0, 0},       // Kernel Code (64-bit)
    {0, 0, 0, 0x92, 0xa0, 0},       // Kernel Data (64-bit)
    {0, 0, 0, 0xfa, 0xa0, 0},       // User Code (64-bit)
    {0, 0, 0, 0xf2, 0xa0, 0},       // User Data (64-bit)
};

void SetupGDT() {
    struct GDTDescriptor gdtDescriptor;
    gdtDescriptor.Size = sizeof(struct GDT) - 1;
    gdtDescriptor.Offset = (uint64_t)&DefaultGDT;

    __asm__ volatile ("lgdt %0" : : "m"(gdtDescriptor));
}
