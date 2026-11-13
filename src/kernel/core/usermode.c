#include "../../include/usermode.h"
#include "../../include/gdt.h"
#include "../../include/pmm.h"
#include "../../include/vmm.h"
#include "../../include/simplefs.h"
#include "../../include/heap.h"
#include <stdint.h>
#include <stddef.h>

void serial_print(const char *str);

#define ELFCLASS64 2u
#define ELFDATA2LSB 1u
#define EM_X86_64 62u
#define ET_EXEC 2u
#define PT_LOAD 1u
#define PF_X 1u
#define PF_W 2u
#define USER_ADDRESS_LIMIT 0x0000800000000000ULL
#define USER_STACK_TOP 0x0000000040000000ULL
#define USER_STACK_PAGES 32u
#define MAX_USER_IMAGE_PAGES 16384u

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

static void mem_copy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

static void mem_zero(void *dst, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = 0;
}

static int elf_header_valid(const Elf64_Ehdr *ehdr, uint32_t file_size) {
    if (!ehdr || file_size < sizeof(*ehdr)) return 0;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') return 0;
    if (ehdr->e_ident[4] != ELFCLASS64 || ehdr->e_ident[5] != ELFDATA2LSB ||
        ehdr->e_ident[6] != 1) return 0;
    if (ehdr->e_type != ET_EXEC || ehdr->e_machine != EM_X86_64 ||
        ehdr->e_version != 1 || ehdr->e_ehsize != sizeof(Elf64_Ehdr) ||
        ehdr->e_phentsize != sizeof(Elf64_Phdr) || ehdr->e_phnum == 0 ||
        ehdr->e_phnum > 128) return 0;

    if (ehdr->e_phoff > file_size) return 0;
    uint64_t ph_bytes = (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
    if (ph_bytes > (uint64_t)file_size - ehdr->e_phoff) return 0;
    return 1;
}

static int load_segment_valid(const Elf64_Phdr *ph, uint32_t file_size) {
    if (ph->p_type != PT_LOAD) return 1;
    if (ph->p_memsz == 0) return ph->p_filesz == 0;
    if (ph->p_filesz > ph->p_memsz || ph->p_offset > file_size ||
        ph->p_filesz > (uint64_t)file_size - ph->p_offset) return 0;
    if (ph->p_vaddr == 0 || ph->p_vaddr >= USER_ADDRESS_LIMIT ||
        ph->p_memsz > USER_ADDRESS_LIMIT - ph->p_vaddr) return 0;
    if (ph->p_memsz > UINT64_MAX - (PAGE_SIZE - 1u)) return 0;

    if (ph->p_align > 1) {
        if ((ph->p_align & (ph->p_align - 1u)) != 0) return 0;
        if ((ph->p_vaddr & (ph->p_align - 1u)) !=
            (ph->p_offset & (ph->p_align - 1u))) return 0;
    }
    return 1;
}

static uint64_t segment_page_start(const Elf64_Phdr *ph) {
    return ph->p_vaddr & ~(uint64_t)(PAGE_SIZE - 1u);
}

static uint64_t segment_page_end(const Elf64_Phdr *ph) {
    uint64_t end = ph->p_vaddr + ph->p_memsz;
    return (end + PAGE_SIZE - 1u) & ~(uint64_t)(PAGE_SIZE - 1u);
}

static void rollback_user_pages(uint64_t *mapped, uint32_t count) {
    while (count > 0) {
        count--;
        VMM_UnmapPage((void *)(uintptr_t)mapped[count], 1);
    }
}

void usermode_launch(void) {
    uint32_t elf_size = 0;
    if (!simplefs_file_size("doom.elf", &elf_size) || elf_size < sizeof(Elf64_Ehdr)) {
        serial_print("[USERMODE] doom.elf missing or too small\n");
        return;
    }

    void *elf_data = kmalloc(elf_size);
    if (!elf_data) {
        serial_print("[USERMODE] failed to allocate ELF buffer\n");
        return;
    }

    uint32_t out_len = 0;
    if (!simplefs_read_file("doom.elf", elf_data, elf_size, &out_len) ||
        out_len != elf_size) {
        serial_print("[USERMODE] failed to read complete doom.elf\n");
        kfree(elf_data);
        return;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;
    if (!elf_header_valid(ehdr, elf_size)) {
        serial_print("[USERMODE] invalid or unsupported ELF header\n");
        kfree(elf_data);
        return;
    }

    Elf64_Phdr *phdrs = (Elf64_Phdr *)((uint8_t *)elf_data + ehdr->e_phoff);
    uint64_t total_pages = 0;
    int entry_is_executable = 0;

    // Validate every segment before allocating or modifying page tables.
    for (uint32_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (!load_segment_valid(ph, elf_size)) {
            serial_print("[USERMODE] invalid ELF load segment\n");
            kfree(elf_data);
            return;
        }
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        uint64_t start = segment_page_start(ph);
        uint64_t end = segment_page_end(ph);
        if (end <= start || (end - start) / PAGE_SIZE > MAX_USER_IMAGE_PAGES ||
            total_pages > MAX_USER_IMAGE_PAGES - ((end - start) / PAGE_SIZE)) {
            serial_print("[USERMODE] ELF image exceeds loader limits\n");
            kfree(elf_data);
            return;
        }
        total_pages += (end - start) / PAGE_SIZE;

        if ((ph->p_flags & PF_X) && ehdr->e_entry >= ph->p_vaddr &&
            ehdr->e_entry < ph->p_vaddr + ph->p_memsz) {
            entry_is_executable = 1;
        }

        // Do not allow two load segments to claim the same virtual page.
        for (uint32_t j = 0; j < i; j++) {
            Elf64_Phdr *other = &phdrs[j];
            if (other->p_type != PT_LOAD || other->p_memsz == 0) continue;
            uint64_t other_start = segment_page_start(other);
            uint64_t other_end = segment_page_end(other);
            if (start < other_end && other_start < end) {
                serial_print("[USERMODE] overlapping ELF load segments\n");
                kfree(elf_data);
                return;
            }
        }
    }

    if (!entry_is_executable || total_pages == 0 ||
        total_pages > SIZE_MAX / sizeof(uint64_t) - USER_STACK_PAGES) {
        serial_print("[USERMODE] ELF entry point is not executable\n");
        kfree(elf_data);
        return;
    }

    uint32_t record_capacity = (uint32_t)total_pages + USER_STACK_PAGES;
    uint64_t *mapped_pages = (uint64_t *)kmalloc(
        (size_t)record_capacity * sizeof(uint64_t));
    if (!mapped_pages) {
        serial_print("[USERMODE] failed to allocate page tracking array\n");
        kfree(elf_data);
        return;
    }
    uint32_t mapped_count = 0;

    // Load each PT_LOAD segment. Pages are initially cleared so that BSS is
    // zero-filled before the initialized portion is copied into place.
    for (uint32_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        uint64_t start = segment_page_start(ph);
        uint64_t end = segment_page_end(ph);
        uint64_t page_flags = PAGE_USER | PAGE_PRESENT;
        if (ph->p_flags & PF_W) page_flags |= PAGE_WRITE;

        for (uint64_t va = start; va < end; va += PAGE_SIZE) {
            void *pa = PMM_AllocatePage();
            if (!pa || !VMM_MapPage((void *)(uintptr_t)va, pa, page_flags)) {
                if (pa) PMM_FreePage(pa);
                rollback_user_pages(mapped_pages, mapped_count);
                kfree(mapped_pages);
                kfree(elf_data);
                serial_print("[USERMODE] unable to map ELF segment pages\n");
                return;
            }
            mapped_pages[mapped_count++] = va;
            mem_zero((void *)(uintptr_t)va, PAGE_SIZE);
        }

        mem_copy((void *)(uintptr_t)ph->p_vaddr,
                 (const uint8_t *)elf_data + ph->p_offset, ph->p_filesz);
    }

    // Create a dedicated user stack.
    uint64_t stack_bottom = USER_STACK_TOP -
                            (uint64_t)USER_STACK_PAGES * PAGE_SIZE;
    for (uint32_t i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t va = stack_bottom + (uint64_t)i * PAGE_SIZE;
        void *pa = PMM_AllocatePage();
        if (!pa || !VMM_MapPage((void *)(uintptr_t)va, pa,
                                PAGE_USER | PAGE_WRITE | PAGE_PRESENT)) {
            if (pa) PMM_FreePage(pa);
            rollback_user_pages(mapped_pages, mapped_count);
            kfree(mapped_pages);
            kfree(elf_data);
            serial_print("[USERMODE] unable to map user stack\n");
            return;
        }
        mapped_pages[mapped_count++] = va;
        mem_zero((void *)(uintptr_t)va, PAGE_SIZE);
    }

    void *kernel_stack = PMM_AllocatePage();
    if (!kernel_stack) {
        rollback_user_pages(mapped_pages, mapped_count);
        kfree(mapped_pages);
        kfree(elf_data);
        serial_print("[USERMODE] no kernel transition stack available\n");
        return;
    }

    if (!VMM_MapPage(kernel_stack, kernel_stack, PAGE_WRITE | PAGE_PRESENT)) {
        PMM_FreePage(kernel_stack);
        rollback_user_pages(mapped_pages, mapped_count);
        kfree(mapped_pages);
        kfree(elf_data);
        serial_print("[USERMODE] unable to map kernel transition stack\n");
        return;
    }

    uint64_t user_entry = ehdr->e_entry;
    kfree(mapped_pages);
    kfree(elf_data);

    TSS_Load((uint64_t)(uintptr_t)kernel_stack + PAGE_SIZE);
    serial_print("[USERMODE] entering ring3\n");

    // A C function entered without CALL must see the ABI's 16-byte
    // stack alignment (RSP % 16 == 8) and a harmless fake return slot.
    uint64_t user_rsp = USER_STACK_TOP - sizeof(uint64_t);
    *(uint64_t *)(uintptr_t)user_rsp = 0;
    uint64_t user_cs = 0x1B;
    uint64_t user_ss = 0x23;

    __asm__ volatile(
        "cli\n"
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
        : [ss] "r"(user_ss), [rsp] "r"(user_rsp),
          [cs] "r"(user_cs), [rip] "r"(user_entry)
        : "rax", "memory");
}
