#include "../include/idt.h"
#include "../include/interrupts.h"
#include <stdint.h>

__attribute__((aligned(0x10)))
static struct IDTEntry idt[256];
static struct IDTR idtr;

extern void irq0_stub();
extern uint64_t Task_Schedule(uint64_t current_rsp);

void SetIDTGate(uint8_t vector, void* handler, uint8_t type_attr) {
    uint64_t offset = (uint64_t)handler;
    idt[vector].Offset0 = (uint16_t)offset;
    idt[vector].Selector = 0x08; // Kernel Code Segment
    idt[vector].IST = 0;
    idt[vector].TypeAttributes = type_attr;
    idt[vector].Offset1 = (uint16_t)(offset >> 16);
    idt[vector].Offset2 = (uint32_t)(offset >> 32);
    idt[vector].Reserved = 0;
}

uint64_t irq0_handler(uint64_t rsp) {
    uint64_t next_rsp = Task_Schedule(rsp);
    PIC_EndMaster();
    return next_rsp;
}

void exception_handler() {
    __asm__ volatile ("cli; hlt");
}

void SetupIDT() {
    idtr.Limit = sizeof(idt) - 1;
    idtr.Offset = (uint64_t)&idt;

    for (int i = 0; i < 32; i++) {
        SetIDTGate(i, exception_handler, 0x8E);
    }

    SetIDTGate(32, irq0_stub, 0x8E); // IRQ0 - Timer

    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

