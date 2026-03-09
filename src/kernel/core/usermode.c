#include "../../include/usermode.h"
#include "../../include/gdt.h"
#include "../../include/pmm.h"
#include "../../include/vmm.h"
#include <stdint.h>

void serial_print(const char *str);

static void mem_copy(void *dst, const void *src, uint64_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  for (uint64_t i = 0; i < n; i++) {
    d[i] = s[i];
  }
}

void usermode_launch(void) {
  // Fixed user virtual addresses (canonical low-half)
  // Must be outside the kernel's early identity-mapped region (first 128MB)
  // because those paging levels were created without PAGE_USER.
  const uint64_t user_code_va = 0x0000000040000000ull;
  const uint64_t user_data_va = 0x0000000040001000ull;
  const uint64_t user_stack_va = 0x0000000040002000ull;

  void *code_pa = PMM_AllocatePage();
  void *data_pa = PMM_AllocatePage();
  void *stack_pa = PMM_AllocatePage();

  if (!code_pa || !data_pa || !stack_pa) {
    serial_print("[USERMODE] out of memory\n");
    return;
  }

  // Map user pages with U/S bit.
  VMM_MapPage((void *)user_code_va, code_pa, PAGE_USER | PAGE_WRITE);
  VMM_MapPage((void *)user_data_va, data_pa, PAGE_USER | PAGE_WRITE);
  VMM_MapPage((void *)user_stack_va, stack_pa, PAGE_USER | PAGE_WRITE);

  // User message in user data page.
  const char *msg = "hello from ring3\n";
  uint64_t msg_len = 17;
  mem_copy((void *)user_data_va, msg, msg_len);

  // Tiny user program machine code (x86_64):
  // mov ax,0x23; mov ds,ax; mov es,ax; mov fs,ax; mov gs,ax;
  // mov rax,1; mov rdi, user_data_va; mov rsi, msg_len; int 0x80; jmp .
  uint8_t prog[64];
  uint32_t n = 0;
  // mov ax,0x23
  prog[n++] = 0x66;
  prog[n++] = 0xB8;
  prog[n++] = 0x23;
  prog[n++] = 0x00;
  // mov ds,ax
  prog[n++] = 0x8E;
  prog[n++] = 0xD8;
  // mov es,ax
  prog[n++] = 0x8E;
  prog[n++] = 0xC0;
  // mov fs,ax
  prog[n++] = 0x8E;
  prog[n++] = 0xE0;
  // mov gs,ax
  prog[n++] = 0x8E;
  prog[n++] = 0xE8;

  // mov rax, 1
  prog[n++] = 0x48;
  prog[n++] = 0xC7;
  prog[n++] = 0xC0;
  prog[n++] = 0x01;
  prog[n++] = 0x00;
  prog[n++] = 0x00;
  prog[n++] = 0x00;

  // mov rdi, imm64
  prog[n++] = 0x48;
  prog[n++] = 0xBF;
  *(uint64_t *)&prog[n] = user_data_va;
  n += 8;

  // mov rsi, imm64
  prog[n++] = 0x48;
  prog[n++] = 0xBE;
  *(uint64_t *)&prog[n] = msg_len;
  n += 8;

  // int 0x80
  prog[n++] = 0xCD;
  prog[n++] = 0x80;

  // mov rax, 0 (sys_exit)
  prog[n++] = 0x48;
  prog[n++] = 0xC7;
  prog[n++] = 0xC0;
  prog[n++] = 0x00;
  prog[n++] = 0x00;
  prog[n++] = 0x00;
  prog[n++] = 0x00;

  // int 0x80 (exit)
  prog[n++] = 0xCD;
  prog[n++] = 0x80;

  mem_copy((void *)user_code_va, prog, n);

  // Setup kernel stack for ring3->ring0 transitions (TSS.rsp0)
  void *kstack = PMM_AllocatePage();
  if (!kstack) {
    serial_print("[USERMODE] no kernel stack\n");
    return;
  }

  // Ensure the stack page is mapped (PMM may return pages outside the initial
  // identity-mapped window).
  VMM_MapPage(kstack, kstack, PAGE_WRITE);

  uint64_t kstack_top = (uint64_t)kstack + 4096ull;
  TSS_Load(kstack_top);

  serial_print("[USERMODE] entering ring3\n");

  uint64_t user_rip = user_code_va;
  uint64_t user_rsp = user_stack_va + 4096ull;

  // Segment selectors (GDT):
  // 0x18 user code, 0x20 user data. Add RPL=3.
  uint64_t user_cs = 0x1Bull;
  uint64_t user_ss = 0x23ull;

  __asm__ volatile("cli\n"
                   "mov $0x23, %%ax\n"
                   "mov %%ax, %%ds\n"
                   "mov %%ax, %%es\n"
                   "mov %%ax, %%fs\n"
                   "mov %%ax, %%gs\n"
                   "pushq %[ss]\n"
                   "pushq %[rsp]\n"
                   "pushq $0x202\n"
                   "pushq %[cs]\n"
                   "pushq %[rip]\n"
                   "iretq\n"
                   :
                   : [ss] "r"(user_ss), [rsp] "r"(user_rsp), [cs] "r"(user_cs),
                     [rip] "r"(user_rip)
                   : "rax", "memory");
}
