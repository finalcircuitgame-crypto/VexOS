#include <stdint.h>
#include "../../include/task.h"

void serial_print(const char *str);
extern void usermode_cleanup_current(void);
extern void syscall_process_cleanup(void);
extern void desktop_user_app_exited(int faulted);

struct InterruptFrameErr {
  uint64_t r15;
  uint64_t r14;
  uint64_t r13;
  uint64_t r12;
  uint64_t r11;
  uint64_t r10;
  uint64_t r9;
  uint64_t r8;
  uint64_t rbp;
  uint64_t rdi;
  uint64_t rsi;
  uint64_t rdx;
  uint64_t rcx;
  uint64_t rbx;
  uint64_t rax;
  uint64_t vector;
  uint64_t error_code;
  uint64_t rip;
  uint64_t cs;
  uint64_t rflags;
  uint64_t rsp;
  uint64_t ss;
} __attribute__((packed));

static void hex64(char out[17], uint64_t v) {
  for (int i = 0; i < 16; i++) {
    uint8_t nib = (uint8_t)((v >> ((15 - i) * 4)) & 0xFu);
    out[i] = (nib < 10) ? (char)('0' + nib) : (char)('A' + (nib - 10));
  }
  out[16] = 0;
}

static void print_exception_details(const struct InterruptFrameErr *f) {
  char h[17];

  serial_print(" vector=");
  hex64(h, f->vector);
  serial_print(h);
  serial_print(" err=");
  hex64(h, f->error_code);
  serial_print(h);

  if (f->vector == 14) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    serial_print(" cr2=");
    hex64(h, cr2);
    serial_print(h);
  }

  serial_print(" rip=");
  hex64(h, f->rip);
  serial_print(h);
  serial_print("\n");
}

static __attribute__((noreturn)) void kernel_panic_halt(void) {
  serial_print("[KERNEL PANIC] Fatal kernel-mode exception. System halted.\n");
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}

uint64_t exception_handler(uint64_t rsp) {
  struct InterruptFrameErr *f = (struct InterruptFrameErr *)rsp;
  if (!f)
    kernel_panic_halt();

  /* The saved CS RPL identifies the privilege level at the time of fault. */
  if ((f->cs & 3u) == 3u) {
    serial_print("[USERFAULT] Terminating ring-3 application:");
    print_exception_details(f);

    /* Reclaim process-owned state while executing on the dedicated kernel
       transition stack. The transition stack itself is freed later, after
       the scheduler has switched back to the kernel task. */
    usermode_cleanup_current();
    syscall_process_cleanup();
    desktop_user_app_exited(1);

    serial_print("[USERFAULT] Application terminated; kernel continues.\n");
    uint64_t next_rsp = Task_Exit(rsp);
    if (!next_rsp)
      kernel_panic_halt();
    return next_rsp;
  }

  serial_print("[KERNEL PANIC] ");
  print_exception_details(f);
  kernel_panic_halt();
}
