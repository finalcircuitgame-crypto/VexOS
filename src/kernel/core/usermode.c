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

#include "../../include/simplefs.h"
#include "../../include/heap.h"

typedef struct {
    unsigned char e_ident[16];
    uint16_t      e_type;
    uint16_t      e_machine;
    uint32_t      e_version;
    uint64_t      e_entry;
    uint64_t      e_phoff;
    uint64_t      e_shoff;
    uint32_t      e_flags;
    uint16_t      e_ehsize;
    uint16_t      e_phentsize;
    uint16_t      e_phnum;
    uint16_t      e_shentsize;
    uint16_t      e_shnum;
    uint16_t      e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t   p_type;
    uint32_t   p_flags;
    uint64_t   p_offset;
    uint64_t   p_vaddr;
    uint64_t   p_paddr;
    uint64_t   p_filesz;
    uint64_t   p_memsz;
    uint64_t   p_align;
} Elf64_Phdr;

void usermode_launch(void) {
  uint32_t elf_size = 0;
  if (simplefs_file_size("doom.elf", &elf_size) == 0) {
      serial_print("[USERMODE] doom.elf not found\n");
      return;
  }
  
  void* elf_data = kmalloc(elf_size);
  if (!elf_data) {
      serial_print("[USERMODE] failed to allocate memory for elf\n");
      return;
  }
  
  uint32_t out_len = 0;
  if (simplefs_read_file("doom.elf", elf_data, elf_size, &out_len) == 0) {
      serial_print("[USERMODE] failed to read doom.elf\n");
      kfree(elf_data);
      return;
  }
  
  Elf64_Ehdr* ehdr = (Elf64_Ehdr*)elf_data;
  if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' || ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
      serial_print("[USERMODE] invalid ELF signature\n");
      kfree(elf_data);
      return;
  }
  
  Elf64_Phdr* phdrs = (Elf64_Phdr*)((uint8_t*)elf_data + ehdr->e_phoff);
  for(int i = 0; i < ehdr->e_phnum; i++) {
      if (phdrs[i].p_type == 1) { // PT_LOAD
          uint64_t memsz = phdrs[i].p_memsz;
          uint64_t filesz = phdrs[i].p_filesz;
          uint64_t vaddr = phdrs[i].p_vaddr;
          uint64_t offset = phdrs[i].p_offset;
          
          uint32_t pages = (memsz + 4095) / 4096;
          for(uint32_t p = 0; p < pages; p++) {
              void* pa = PMM_AllocatePage();
              uint64_t va_page = (vaddr + p * 4096) & ~4095ULL;
              VMM_MapPage((void*)va_page, pa, PAGE_USER | PAGE_WRITE | PAGE_PRESENT);
              
              // Zero page first
              for(int j=0; j<4096; j++) ((uint8_t*)va_page)[j] = 0;
          }
          
          // Copy data
          mem_copy((void*)vaddr, (uint8_t*)elf_data + offset, filesz);
      }
  }
  
  uint64_t user_rip = ehdr->e_entry;
  kfree(elf_data);

  // Setup user stack
  const uint64_t user_stack_va = 0x0000000040000000ull; // 1GB boundary for stack
  for(int i=0; i<32; i++) {
      void* stack_pa = PMM_AllocatePage();
      VMM_MapPage((void*)(user_stack_va - 4096*(i+1)), stack_pa, PAGE_USER | PAGE_WRITE | PAGE_PRESENT);
  }

  // Setup kernel stack for ring3->ring0 transitions (TSS.rsp0)
  void *kstack = PMM_AllocatePage();
  if (!kstack) {
    serial_print("[USERMODE] no kernel stack\n");
    return;
  }

  VMM_MapPage(kstack, kstack, PAGE_WRITE | PAGE_PRESENT);

  uint64_t kstack_top = (uint64_t)kstack + 4096ull;
  TSS_Load(kstack_top);

  serial_print("[USERMODE] entering ring3\n");

  uint64_t user_rsp = user_stack_va;

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
