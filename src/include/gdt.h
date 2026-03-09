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

struct TSS64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct GDT {
    struct GDTEntry Null;
    struct GDTEntry KernelCode;
    struct GDTEntry KernelData;
    struct GDTEntry UserCode;
    struct GDTEntry UserData;
    struct GDTEntry TSSLow;
    uint32_t TSSHigh;
    uint32_t TSSReserved;
} __attribute__((packed)) __attribute__((aligned(0x1000)));

void SetupGDT(void);
void TSS_Init(uint64_t ist1);
void TSS_Load(uint64_t rsp0);

#endif
