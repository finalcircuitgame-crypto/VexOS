#include "../../include/interrupts.h"
#include <stdint.h>

void serial_print(const char *str);
extern uint64_t Task_Exit(uint64_t current_rsp);

static void sys_write_user(const char *s, uint64_t n) {
  if (!s)
    return;

  // Minimal v0: assume the user pointer is mapped and readable.
  // Print up to a reasonable limit to avoid runaway.
  uint64_t maxn = n;
  if (maxn > 4096ull)
    maxn = 4096ull;

  for (uint64_t i = 0; i < maxn; i++) {
    char c = s[i];
    char t[2] = {c, 0};
    serial_print(t);
  }
}

uint64_t syscall_handler(uint64_t rsp) {
  struct InterruptFrame *f = (struct InterruptFrame *)rsp;

  // Syscall convention (v0):
  // rax = syscall number
  // rdi = arg0
  // rsi = arg1
  // return in rax
  uint64_t num = f->rax;
  if (num == 0) {
    // sys_exit: terminate the current task
    serial_print("[USERMODE] exiting ring3\n");
    return Task_Exit(rsp);
  } else if (num == 1) {
    sys_write_user((const char *)f->rdi, f->rsi);
    f->rax = 0;
  } else {
    f->rax = (uint64_t)-1;
  }

  return rsp;
}
