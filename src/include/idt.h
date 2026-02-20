#ifndef IDT_H
#define IDT_H

#include <stdint.h>

struct IDTEntry {
    uint16_t Offset0;
    uint16_t Selector;
    uint8_t  IST;
    uint8_t  TypeAttributes;
    uint16_t Offset1;
    uint32_t Offset2;
    uint32_t Reserved;
} __attribute__((packed));

struct IDTR {
    uint16_t Limit;
    uint64_t Offset;
} __attribute__((packed));

void SetIDTGate(uint8_t vector, void* handler, uint8_t type_attr);
void SetupIDT();

#endif
