#include "../../include/gdt.h"
#include <stdint.h>

static struct TSS64 g_tss;
static uint8_t g_ist1_stack[4096] __attribute__((aligned(16)));

struct GDT DefaultGDT = {
    {0, 0, 0, 0, 0, 0},             // Null
    {0, 0, 0, 0x9a, 0xa0, 0},       // Kernel Code (64-bit)
    {0, 0, 0, 0x92, 0xa0, 0},       // Kernel Data (64-bit)
    {0, 0, 0, 0xfa, 0xa0, 0},       // User Code (64-bit)
    {0, 0, 0, 0xf2, 0xa0, 0},       // User Data (64-bit)
    {0, 0, 0, 0, 0, 0},             // TSS Low (filled in at runtime)
    0,
    0,
};

void SetupGDT() {
    struct GDTDescriptor gdtDescriptor;
    gdtDescriptor.Size = sizeof(struct GDT) - 1;
    gdtDescriptor.Offset = (uint64_t)&DefaultGDT;

    __asm__ volatile (
        "lgdt %0\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        :
        : "m"(gdtDescriptor)
        : "rax", "memory");
}

void TSS_Init(uint64_t ist1) {
    for (uint32_t i = 0; i < (uint32_t)sizeof(g_tss); i++) {
        ((uint8_t *)&g_tss)[i] = 0;
    }

    if (ist1 == 0) {
        ist1 = (uint64_t)&g_ist1_stack[4096];
    }
    g_tss.ist1 = ist1;
    g_tss.iopb_offset = (uint16_t)sizeof(struct TSS64);

    uint64_t base = (uint64_t)&g_tss;
    uint32_t limit = (uint32_t)sizeof(struct TSS64) - 1u;

    // 64-bit available TSS descriptor (type=0x9), present, DPL=0.
    DefaultGDT.TSSLow.Limit0 = (uint16_t)(limit & 0xFFFFu);
    DefaultGDT.TSSLow.Base0 = (uint16_t)(base & 0xFFFFu);
    DefaultGDT.TSSLow.Base1 = (uint8_t)((base >> 16) & 0xFFu);
    DefaultGDT.TSSLow.Access = 0x89;
    DefaultGDT.TSSLow.Limit1_Flags = (uint8_t)(((limit >> 16) & 0x0Fu));
    DefaultGDT.TSSLow.Base2 = (uint8_t)((base >> 24) & 0xFFu);

    DefaultGDT.TSSHigh = (uint32_t)(base >> 32);
    DefaultGDT.TSSReserved = 0;

    // TSS selector = 0x28 (GDT entry 5) with RPL=0.
    __asm__ volatile ("ltr %0" : : "r"((uint16_t)0x28));
}

void TSS_Load(uint64_t rsp0) {
    g_tss.rsp0 = rsp0;
}
