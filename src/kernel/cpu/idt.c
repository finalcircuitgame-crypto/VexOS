#include "../../include/idt.h"
#include "../../include/interrupts.h"
#include <stdint.h>

__attribute__((aligned(0x10))) static struct IDTEntry idt[256];
static struct IDTR idtr;

extern void irq0_stub();
extern void syscall_stub();
extern void isr0_stub();
extern void isr1_stub();
extern void isr2_stub();
extern void isr3_stub();
extern void isr4_stub();
extern void isr5_stub();
extern void isr6_stub();
extern void isr7_stub();
extern void isr8_stub();
extern void isr9_stub();
extern void isr10_stub();
extern void isr11_stub();
extern void isr12_stub();
extern void isr13_stub();
extern void isr14_stub();
extern void isr15_stub();
extern void isr16_stub();
extern void isr17_stub();
extern void isr18_stub();
extern void isr19_stub();
extern void isr20_stub();
extern void isr21_stub();
extern void isr22_stub();
extern void isr23_stub();
extern void isr24_stub();
extern void isr25_stub();
extern void isr26_stub();
extern void isr27_stub();
extern void isr28_stub();
extern void isr29_stub();
extern void isr30_stub();
extern void isr31_stub();
extern uint64_t Task_Schedule(uint64_t current_rsp);

void SetIDTGate(uint8_t vector, void *handler, uint8_t type_attr) {
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
  Timer_OnTick();
  rsp = Task_Schedule(rsp);
  PIC_EndMaster();
  return rsp;
}

static void exception_halt_handler() { __asm__ volatile("cli; hlt"); }

void SetupIDT() {
  idtr.Limit = sizeof(idt) - 1;
  idtr.Offset = (uint64_t)&idt;

  SetIDTGate(0, isr0_stub, 0x8E);
  SetIDTGate(1, isr1_stub, 0x8E);
  SetIDTGate(2, isr2_stub, 0x8E);
  SetIDTGate(3, isr3_stub, 0x8E);
  SetIDTGate(4, isr4_stub, 0x8E);
  SetIDTGate(5, isr5_stub, 0x8E);
  SetIDTGate(6, isr6_stub, 0x8E);
  SetIDTGate(7, isr7_stub, 0x8E);
  SetIDTGate(8, isr8_stub, 0x8E);
  SetIDTGate(9, isr9_stub, 0x8E);
  SetIDTGate(10, isr10_stub, 0x8E);
  SetIDTGate(11, isr11_stub, 0x8E);
  SetIDTGate(12, isr12_stub, 0x8E);
  SetIDTGate(13, isr13_stub, 0x8E);
  SetIDTGate(14, isr14_stub, 0x8E);
  SetIDTGate(15, isr15_stub, 0x8E);
  SetIDTGate(16, isr16_stub, 0x8E);
  SetIDTGate(17, isr17_stub, 0x8E);
  SetIDTGate(18, isr18_stub, 0x8E);
  SetIDTGate(19, isr19_stub, 0x8E);
  SetIDTGate(20, isr20_stub, 0x8E);
  SetIDTGate(21, isr21_stub, 0x8E);
  SetIDTGate(22, isr22_stub, 0x8E);
  SetIDTGate(23, isr23_stub, 0x8E);
  SetIDTGate(24, isr24_stub, 0x8E);
  SetIDTGate(25, isr25_stub, 0x8E);
  SetIDTGate(26, isr26_stub, 0x8E);
  SetIDTGate(27, isr27_stub, 0x8E);
  SetIDTGate(28, isr28_stub, 0x8E);
  SetIDTGate(29, isr29_stub, 0x8E);
  SetIDTGate(30, isr30_stub, 0x8E);
  SetIDTGate(31, isr31_stub, 0x8E);

  SetIDTGate(32, irq0_stub, 0x8E); // IRQ0 - Timer

  // Syscall gate (int 0x80), user-callable (DPL=3)
  SetIDTGate(0x80, syscall_stub, 0xEE);

  __asm__ volatile("lidt %0" : : "m"(idtr));
}
