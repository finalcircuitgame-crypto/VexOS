#include "../include/interrupts.h"
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void PIC_EndMaster() {
    outb(0x20, 0x20);
}

void PIC_Remap() {
    uint8_t a1, a2;

    // Save masks
    __asm__ volatile ("inb $0x21, %0" : "=a"(a1));
    __asm__ volatile ("inb $0xA1, %0" : "=a"(a2));

    outb(0x20, 0x11); // ICW1_INIT | ICW1_ICW4
    outb(0xA0, 0x11);
    outb(0x21, 0x20); // Master offset 32
    outb(0xA1, 0x28); // Slave offset 40
    outb(0x21, 0x04); // ICW3_MASTER_SLAVE
    outb(0xA1, 0x02); // ICW3_SLAVE_ID
    outb(0x21, 0x01); // ICW4_8086
    outb(0xA1, 0x01);

    // Restore masks
    outb(0x21, a1);
    outb(0xA1, a2);
}

void PIT_Init(uint32_t frequency) {
    uint32_t divisor = 1193182 / frequency;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}
