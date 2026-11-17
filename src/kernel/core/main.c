#include "bootinfo.h"
#include "bootlog.h"
#include "ahci.h"
#include "desktop.h"
#include "gdt.h"
#include "heap.h"
#include "hw_profile.h"
#include "idt.h"
#include "pmm.h"
#include "simplefs.h"
#include "sfs2.h"
#include "vfs.h"
#include "task.h"
#include "process.h"
#include "vmm.h"
#include "../desktop/graphics/font_engine.h"
#include "pci.h"
#include "netdev.h"
#include "ethernet.h"
#include "arp.h"
#include <stddef.h>
#include <stdint.h>

void xhci_poll_events(void);

/* xHCI polling is shared by the desktop and user-mode input syscalls.
   Keep the event-ring walk atomic against the PIT task switch. */
static void xhci_poll_events_atomic(void) {
  uint64_t flags;
  __asm__ volatile("pushfq\n\tcli\n\tpopq %0"
                   : "=r"(flags) :: "memory");
  xhci_poll_events();
  if (flags & (1ull << 9))
    __asm__ volatile("sti" ::: "memory");
}

#ifndef VEXOS_VERSION
#define VEXOS_VERSION "2.0.0-dev"
#endif
#ifndef VEXOS_BUILD_ID
#define VEXOS_BUILD_ID "unknown"
#endif

// Defined in other files
void ConsoleInit(BootInfo *bootInfo);
void PrintString(const char *str, uint32_t color);
void SetupGDT();
void SetupIDT();
void PMM_FreePages(void *addr, uint64_t count);

// Serial Port output
static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static inline uint16_t pit_read_counter(void) {
  // Latch channel 0 count, then read low/high.
  outb(0x43, 0x00);
  uint8_t lo = inb(0x40);
  uint8_t hi = inb(0x40);
  return (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

static int g_ConsoleReady = 0;

void serial_print(const char *str) {
  /* The PIT can preempt a task between serial characters and let another
     task interleave its message. COM1 output is short; serialize it on this
     single-core kernel by preserving the caller's interrupt-enable state. */
  uint64_t irq_flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(irq_flags) :: "memory");

  /* Capture every serial message before attempting hardware output. */
  bootlog_append(str);

  // Always print to COM1 as one indivisible message.
  const char *p = str;
  while (*p) {
    outb(0x3F8, *p++);
  }

  if (irq_flags & (1ull << 9))
    __asm__ volatile("sti" ::: "memory");

  // Also print to framebuffer console once initialized.
  if (g_ConsoleReady) {
    PrintString(str, 0xFFFFFF);
  }
}

static void vmm_map_or_halt(void *virtual_addr, void *physical_addr,
                            uint64_t flags) {
  if (!VMM_MapPage(virtual_addr, physical_addr, flags)) {
    serial_print("[KERNEL] FATAL: page mapping failed\n");
    while (1) { __asm__ volatile("cli; hlt"); }
  }
}

static int pci_has_qemu_stdvga(void) {
  /* QEMU's standard VGA adapter is PCI 1234:1111. Detect it before VMM
     activation so its second scanout page can be identity-mapped as well. */
  for (uint16_t bus = 0; bus < 256u; ++bus) {
    for (uint8_t slot = 0; slot < 32u; ++slot) {
      for (uint8_t func = 0; func < 8u; ++func) {
        uint32_t id = pci_config_read_dword((uint8_t)bus, slot, func, 0x00u);
        if ((uint16_t)id == 0xFFFFu)
          continue;
        if ((uint16_t)id == 0x1234u &&
            (uint16_t)(id >> 16) == 0x1111u)
          return 1;
      }
    }
  }
  return 0;
}

static uint16_t qemu_vbe_read(uint16_t index) {
  __asm__ volatile("outw %0, %1" : : "a"(index), "Nd"((uint16_t)0x01CEu));
  uint16_t value;
  __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"((uint16_t)0x01CFu));
  return value;
}


#include "interrupts.h"

void PIC_Remap();

void taskA() {
  while (1) {
    PrintString(" [A] ", 0x00FFFF);
    serial_print("A");
    for (volatile int i = 0; i < 5000000; i++)
      ;
  }
}

void taskB() {
  while (1) {
    PrintString(" [B] ", 0xFFFF00);
    serial_print("B");
    for (volatile int i = 0; i < 5000000; i++)
      ;
  }
}

#ifdef VEXOS_CI_INPUT_TEST
static void ci_memory_test_fail(const char *reason) {
  serial_print("[KERNEL] FATAL: CI memory self-test: ");
  serial_print(reason);
  serial_print("\n");
  for (;;) { __asm__ volatile("cli; hlt"); }
}

static void ci_ahci_self_test(void) {
  static uint8_t write_buffer[1024] __attribute__((aligned(16)));
  static uint8_t read_buffer[1024] __attribute__((aligned(16)));

  if (!ahci_is_ready())
    ci_memory_test_fail("AHCI controller or SATA test disk not detected");

  uint64_t sectors = ahci_get_sector_count();
  if (sectors < 32u)
    ci_memory_test_fail("AHCI test disk is unexpectedly small");

  for (uint32_t i = 0; i < sizeof(write_buffer); ++i)
    write_buffer[i] = (uint8_t)((i * 53u + 29u) & 0xFFu);

  // The CI image is a dedicated disposable SATA disk. Keep the test away
  // from LBA 0 and the first few sectors in case metadata is added later.
  const uint64_t test_lba = 16u;
  if (!ahci_write_sectors(test_lba, 2u, write_buffer))
    ci_memory_test_fail("AHCI two-sector DMA write");

  for (uint32_t i = 0; i < sizeof(read_buffer); ++i)
    read_buffer[i] = 0;

  if (!ahci_read_sectors(test_lba, 2u, read_buffer))
    ci_memory_test_fail("AHCI two-sector DMA read");

  for (uint32_t i = 0; i < sizeof(write_buffer); ++i) {
    if (read_buffer[i] != write_buffer[i])
      ci_memory_test_fail("AHCI DMA read-after-write data mismatch");
  }

  if (ahci_read_sectors(sectors, 1u, read_buffer))
    ci_memory_test_fail("AHCI accepted an out-of-range read");
  if (ahci_read_sectors(0u, 0u, read_buffer))
    ci_memory_test_fail("AHCI accepted a zero-sector read");

  serial_print("[CI] AHCI SATA identify, two-sector DMA write/read, and bounds checks passed\n");
}

static void ci_memory_self_test(void) {
  // Heap: alignment, overflow/zero rejection, invalid/double-free tolerance,
  // and coalescing of adjacent blocks.
  void *h1 = kmalloc(1);
  void *h2 = kmalloc(513);
  void *h3 = kmalloc(4097);
  if (!h1 || !h2 || !h3) ci_memory_test_fail("heap allocation");
  if (((uintptr_t)h1 & 15u) || ((uintptr_t)h2 & 15u) ||
      ((uintptr_t)h3 & 15u)) ci_memory_test_fail("heap alignment");
  if (kmalloc(0) != NULL || kmalloc((size_t)-1) != NULL)
    ci_memory_test_fail("heap invalid-size rejection");
  ((uint8_t *)h1)[0] = 0xA5u;
  ((uint8_t *)h2)[512] = 0x5Au;
  ((uint8_t *)h3)[4096] = 0xC3u;
  if (((uint8_t *)h1)[0] != 0xA5u ||
      ((uint8_t *)h2)[512] != 0x5Au ||
      ((uint8_t *)h3)[4096] != 0xC3u)
    ci_memory_test_fail("heap data integrity");
  kfree(h2);
  kfree(h2); // A double free must be ignored.
  kfree((void *)(uintptr_t)0x12345u); // A foreign pointer must be ignored.
  kfree(h1);
  kfree(h3);
  void *merged = kmalloc(8192);
  if (!merged) ci_memory_test_fail("heap free-block coalescing");
  kfree(merged);
  serial_print("[CI] Heap allocator alignment/free/coalesce passed\n");

  // PMM: page alignment, misaligned-free rejection, double-free safety, and reuse.
  void *p1 = PMM_AllocatePage();
  if (!p1 || ((uintptr_t)p1 & 0xFFFu))
    ci_memory_test_fail("PMM page allocation/alignment");
  PMM_FreePage((void *)((uintptr_t)p1 + 1u));
  void *p2 = PMM_AllocatePage();
  if (!p2 || p2 == p1)
    ci_memory_test_fail("PMM misaligned free was accepted");
  PMM_FreePage(p1);
  PMM_FreePage(p1); // A duplicate free must not corrupt the bitmap.
  PMM_FreePage(p2);
  void *p3 = PMM_AllocatePage();
  void *p4 = PMM_AllocatePage();
  if (p3 != p1 || p4 != p2)
    ci_memory_test_fail("PMM freed-page reuse/double-free handling");
  PMM_FreePage(p3);
  PMM_FreePage(p4);
  serial_print("[CI] PMM alignment/misaligned-free/double-free/reuse passed\n");

  // VMM: supervisor/user mappings, range permissions, alignment, and teardown.
  const uintptr_t test_va = 0x0000400000000000ull;
  void *phys1 = PMM_AllocatePage();
  void *phys2 = PMM_AllocatePage();
  if (!phys1 || !phys2) ci_memory_test_fail("VMM backing-page allocation");
  if (!VMM_MapPage((void *)test_va, phys1, PAGE_WRITE))
    ci_memory_test_fail("VMM supervisor map");
  volatile uint64_t *mapped = (volatile uint64_t *)test_va;
  *mapped = 0x5645584F53434931ull;
  if (*mapped != 0x5645584F53434931ull)
    ci_memory_test_fail("VMM mapped-page data integrity");
  if (VMM_MapPage((void *)test_va, phys2, PAGE_WRITE))
    ci_memory_test_fail("VMM conflicting remap accepted");
  if (VMM_IsUserRange((void *)test_va, sizeof(uint64_t), 0))
    ci_memory_test_fail("VMM supervisor page accepted as user memory");
  if (VMM_UnmapPage((void *)test_va, 1))
    ci_memory_test_fail("VMM freed a supervisor page");
  if (*mapped != 0x5645584F53434931ull)
    ci_memory_test_fail("VMM rejected unmap changed mapping");
  if (!VMM_UnmapPage((void *)test_va, 0))
    ci_memory_test_fail("VMM supervisor unmap");
  if (VMM_UnmapPage((void *)test_va, 0))
    ci_memory_test_fail("VMM unmapped a missing page");
  PMM_FreePage(phys1);
  PMM_FreePage(phys2);

  void *user_phys = PMM_AllocatePage();
  void *readonly_phys = PMM_AllocatePage();
  if (!user_phys || !readonly_phys ||
      !VMM_MapPage((void *)test_va, user_phys, PAGE_WRITE | PAGE_USER))
    ci_memory_test_fail("VMM user map");
  if (!VMM_IsUserRange((void *)test_va, PAGE_SIZE, 1))
    ci_memory_test_fail("VMM valid writable user range rejected");
  if (VMM_IsUserRange((void *)test_va, PAGE_SIZE + 1u, 0))
    ci_memory_test_fail("VMM range crossing into an unmapped page accepted");
  if (!VMM_MapPage((void *)(test_va + PAGE_SIZE), readonly_phys, PAGE_USER))
    ci_memory_test_fail("VMM read-only user map");
  if (!VMM_IsUserRange((void *)test_va, PAGE_SIZE * 2u, 0))
    ci_memory_test_fail("VMM valid two-page user range rejected");
  if (VMM_IsUserRange((void *)test_va, PAGE_SIZE * 2u, 1))
    ci_memory_test_fail("VMM writable range accepted a read-only page");
  if (!VMM_UnmapPage((void *)(test_va + PAGE_SIZE), 1))
    ci_memory_test_fail("VMM read-only user unmap/free");
  if (!VMM_UnmapPage((void *)test_va, 1))
    ci_memory_test_fail("VMM user unmap/free");
  if (VMM_IsUserRange((void *)test_va, 1, 0))
    ci_memory_test_fail("VMM unmapped user range accepted");
  if (VMM_MapPage((void *)test_va, (void *)((uintptr_t)user_phys + 1u), PAGE_WRITE))
    ci_memory_test_fail("VMM misaligned physical address accepted");
  if (VMM_MapPage((void *)(test_va + 1u), user_phys, PAGE_WRITE))
    ci_memory_test_fail("VMM misaligned virtual address accepted");
  if (VMM_MapPage((void *)0xFFFF800000000000ull, user_phys, PAGE_WRITE | PAGE_USER))
    ci_memory_test_fail("VMM supervisor address accepted as user memory");
  if (VMM_MapPage((void *)0x0000800000000000ull, user_phys, PAGE_WRITE))
    ci_memory_test_fail("VMM noncanonical virtual address accepted");
  if (VMM_UnmapPage((void *)0x0000800000000000ull, 0))
    ci_memory_test_fail("VMM noncanonical unmap accepted");
  if (VMM_IsUserRange((void *)0x00007FFFFFFFF000ull, PAGE_SIZE * 2u, 0))
    ci_memory_test_fail("VMM range crossing user limit accepted");
  serial_print("[CI] VMM user ranges/write permissions/alignment/unmapping passed\n");
}
#ifdef VEXOS_CI_INPUT_TEST
static void ci_address_space_self_test(void) {
  const uintptr_t test_va = 0x0000600000000000ull;
  const uint64_t value_a = 0x5645584F53504131ull;
  const uint64_t value_b = 0x5645584F53504232ull;
  AddressSpace *space_a = VMM_AddressSpaceCreate();
  AddressSpace *space_b = VMM_AddressSpaceCreate();
  if (!space_a || !space_b)
    ci_memory_test_fail("address-space creation/independent page-table clone");

  void *page_a = PMM_AllocatePage();
  void *page_b = PMM_AllocatePage();
  if (!page_a || !page_b)
    ci_memory_test_fail("address-space backing-page allocation");
  for (uint32_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); ++i) {
    ((uint64_t *)page_a)[i] = 0;
    ((uint64_t *)page_b)[i] = 0;
  }

  if (!VMM_AddressSpaceMapPage(space_a, (void *)test_va, page_a,
                               PAGE_USER | PAGE_WRITE | PAGE_PRESENT) ||
      !VMM_AddressSpaceMapPage(space_b, (void *)test_va, page_b,
                               PAGE_USER | PAGE_WRITE | PAGE_PRESENT))
    ci_memory_test_fail("same virtual address mapping in separate spaces");

  if (VMM_AddressSpaceMapPage(space_a,
          (void *)(test_va + PAGE_SIZE), page_a,
          PAGE_USER | PAGE_WRITE | PAGE_PRESENT))
    ci_memory_test_fail("duplicate owned user frame alias accepted");

  if (!VMM_AddressSpaceIsUserRange(space_a, (void *)test_va, PAGE_SIZE, 1) ||
      !VMM_AddressSpaceIsUserRange(space_b, (void *)test_va, PAGE_SIZE, 1))
    ci_memory_test_fail("address-space user range validation");
  if (VMM_AddressSpaceIsUserRange(space_a, (void *)0x1000u, 1, 0) ||
      VMM_AddressSpaceIsUserRange(space_b, (void *)0x1000u, 1, 0))
    ci_memory_test_fail("kernel identity mapping became user-accessible");

  VMM_ActivateAddressSpace(space_a);
  *(volatile uint64_t *)test_va = value_a;
  VMM_ActivateAddressSpace(space_b);
  if (*(volatile uint64_t *)test_va != 0)
    ci_memory_test_fail("address spaces unexpectedly share user memory");
  *(volatile uint64_t *)test_va = value_b;
  VMM_ActivateAddressSpace(space_a);
  if (*(volatile uint64_t *)test_va != value_a)
    ci_memory_test_fail("address-space A memory changed after switching to B");
  VMM_ActivateAddressSpace(space_b);
  if (*(volatile uint64_t *)test_va != value_b)
    ci_memory_test_fail("address-space B memory was not preserved");

  VMM_Activate();
  VMM_AddressSpaceDestroy(space_a);
  VMM_AddressSpaceDestroy(space_b);
  serial_print("[CI] Independent address spaces, CR3 switching, user-memory isolation, and kernel supervisor mappings passed\n");
}
#endif

extern uint64_t syscall_handler(uint64_t rsp);
extern int usermode_ci_self_test(void);
extern int Task_CI_SelfTest(void);
extern int simplefs_ci_validate_metadata_self_test(void);


#ifdef VEXOS_CI_INPUT_TEST
typedef struct {
  uint8_t ident[16];
  uint16_t type;
  uint16_t machine;
  uint32_t version;
  uint64_t entry;
  uint64_t phoff;
  uint64_t shoff;
  uint32_t flags;
  uint16_t ehsize;
  uint16_t phentsize;
  uint16_t phnum;
  uint16_t shentsize;
  uint16_t shnum;
  uint16_t shstrndx;
} __attribute__((packed)) ci_elf64_ehdr_t;

typedef struct {
  uint32_t type;
  uint32_t flags;
  uint64_t offset;
  uint64_t vaddr;
  uint64_t paddr;
  uint64_t filesz;
  uint64_t memsz;
  uint64_t align;
} __attribute__((packed)) ci_elf64_phdr_t;

static void ci_install_ring3_elf_fixture(void) {
  static uint8_t image[0x1100];
  static const char message[] =
      "[CI] Ring3 int80 syscall and exit passed\n";
  const uint32_t message_len = (uint32_t)sizeof(message) - 1u;
  const uint32_t data_offset = 0x20u;
  const uint32_t code_len = 31u;
  const uint32_t file_size = 0x1000u + data_offset + message_len;

  for (uint32_t i = 0; i < sizeof(image); i++)
    image[i] = 0;

  ci_elf64_ehdr_t *eh = (ci_elf64_ehdr_t *)image;
  eh->ident[0] = 0x7F;
  eh->ident[1] = 'E';
  eh->ident[2] = 'L';
  eh->ident[3] = 'F';
  eh->ident[4] = 2;
  eh->ident[5] = 1;
  eh->ident[6] = 1;
  eh->type = 2;
  eh->machine = 62;
  eh->version = 1;
  eh->entry = 0x4000000000ull;
  eh->phoff = sizeof(*eh);
  eh->ehsize = sizeof(*eh);
  eh->phentsize = sizeof(ci_elf64_phdr_t);
  eh->phnum = 1;

  ci_elf64_phdr_t *ph = (ci_elf64_phdr_t *)(image + eh->phoff);
  ph->type = 1;       // PT_LOAD
  ph->flags = 5;      // PF_R | PF_X
  ph->offset = 0x1000u;
  ph->vaddr = 0x4000000000ull;
  ph->paddr = 0x4000000000ull;
  ph->filesz = data_offset + message_len;
  ph->memsz = PAGE_SIZE;
  ph->align = PAGE_SIZE;

  // mov eax, 1; movabs rdi, 0x4000000020; mov esi, message_len;
  // int 0x80; mov eax, 0; int 0x80; jmp $.
  uint8_t code[31] = {
      0xB8, 0x01, 0x00, 0x00, 0x00,
      0x48, 0xBF, 0x20, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
      0xBE, 0, 0, 0, 0,
      0xCD, 0x80,
      0xB8, 0x00, 0x00, 0x00, 0x00,
      0xCD, 0x80,
      0xEB, 0xFE
  };
  code[16] = (uint8_t)(message_len & 0xFFu);
  code[17] = (uint8_t)((message_len >> 8) & 0xFFu);
  code[18] = (uint8_t)((message_len >> 16) & 0xFFu);
  code[19] = (uint8_t)((message_len >> 24) & 0xFFu);

  for (uint32_t i = 0; i < code_len; i++)
    image[0x1000u + i] = code[i];
  for (uint32_t i = 0; i < message_len; i++)
    image[0x1000u + data_offset + i] = (uint8_t)message[i];

  if (!simplefs_write_file("ci-ring3.elf", image, file_size))
    ci_memory_test_fail("write ring3 ELF fixture to SimpleFS");
  serial_print("[CI] Valid ring3 ELF fixture installed in SimpleFS\n");
}
#endif

static void ci_syscall_self_test(void) {
  struct InterruptFrame frame = {0};

  // CI prepares a small SFS1 image with ci.txt on the ATA primary slave.
  if (!simplefs_init())
    ci_memory_test_fail("SimpleFS fixture mount");
  if (!simplefs_ci_validate_metadata_self_test())
    ci_memory_test_fail("SFS1 metadata validation and extent safety");
  serial_print("[CI] SFS1 metadata names, path conflicts, and extent bounds passed\n");
  if (!sfs2_ci_metadata_self_test())
    ci_memory_test_fail("SFS2 superblock, inode, and bitmap metadata");
  serial_print("[CI] SFS2 superblock, inode, and bitmap metadata passed\n");
  if (!sfs2_ci_mount_read_test())
    ci_memory_test_fail("SFS2 mount, journal recovery, and file mutation");
  serial_print("[CI] SFS2 mount, journal recovery, and file mutation passed\n");
  if (!vfs_init() || !vfs_is_mounted())
    ci_memory_test_fail("VFS root mount over existing SimpleFS");

  /* VFS hierarchy maps slash-separated paths into SFS1's existing name field.
     This preserves the on-disk structure while giving files implicit folders. */
  char normalized[VFS_PATH_MAX];
  static const char normalized_nested[] = "docs/ci.txt";
  if (!vfs_normalize_path("./ci.txt", normalized, sizeof(normalized)) ||
      normalized[0] != 'c' || normalized[1] != 'i')
    ci_memory_test_fail("VFS relative path normalization");
  if (!vfs_normalize_path(".", normalized, sizeof(normalized)) ||
      normalized[0] != '/' || normalized[1] != '\0' ||
      !vfs_normalize_path("/./", normalized, sizeof(normalized)) ||
      normalized[0] != '/' || normalized[1] != '\0')
    ci_memory_test_fail("VFS current-directory normalization");
  if (!vfs_normalize_path("/docs//./ci.txt", normalized, sizeof(normalized)))
    ci_memory_test_fail("VFS nested path normalization");
  for (uint32_t i = 0; i < sizeof(normalized_nested); ++i) {
    if (normalized[i] != normalized_nested[i])
      ci_memory_test_fail("VFS normalized nested path content");
  }
  if (vfs_normalize_path("../ci.txt", normalized, sizeof(normalized)) ||
      vfs_normalize_path("/docs/../ci.txt", normalized, sizeof(normalized)))
    ci_memory_test_fail("VFS parent traversal rejection");
  char unterminated_path[VFS_PATH_MAX];
  for (uint32_t i = 0; i < sizeof(unterminated_path); ++i)
    unterminated_path[i] = 'a';
  if (vfs_normalize_path(unterminated_path, normalized, sizeof(normalized)))
    ci_memory_test_fail("VFS bounded path length");

  vfs_stat_t vfs_info;
  if (!vfs_stat("/ci.txt", &vfs_info) ||
      vfs_info.type != VFS_NODE_FILE || vfs_info.size != 700u)
    ci_memory_test_fail("VFS stat on pre-existing SimpleFS file");

  static const char vfs_payload[] = "VexOS VFS read/write regression";
  char vfs_readback[sizeof(vfs_payload)];
  uint32_t vfs_read_len = 0;
  if (!vfs_write("/ci-vfs.txt", vfs_payload, sizeof(vfs_payload)) ||
      !vfs_read("/ci-vfs.txt", vfs_readback, sizeof(vfs_readback),
                &vfs_read_len) ||
      vfs_read_len != sizeof(vfs_payload))
    ci_memory_test_fail("VFS write and read through SimpleFS backend");
  for (uint32_t i = 0; i < sizeof(vfs_payload); ++i) {
    if (vfs_readback[i] != vfs_payload[i])
      ci_memory_test_fail("VFS read/write content mismatch");
  }
  char vfs_slice[8];
  uint32_t vfs_slice_len = 0;
  if (!vfs_read_at("/ci-vfs.txt", 6u, vfs_slice, 7u, &vfs_slice_len) ||
      vfs_slice_len != 7u || vfs_slice[0] != 'V' ||
      vfs_slice[6] != 'a')
    ci_memory_test_fail("VFS ranged read");

  static const char nested_payload[] = "VFS nested file survives";
  if (!vfs_write("/docs/readme.txt", nested_payload, sizeof(nested_payload)))
    ci_memory_test_fail("VFS nested file write");
  vfs_stat_t directory_stat;
  if (!vfs_stat("/docs", &directory_stat) ||
      directory_stat.type != VFS_NODE_DIRECTORY ||
      !vfs_stat("/docs/readme.txt", &vfs_info) ||
      vfs_info.type != VFS_NODE_FILE ||
      vfs_info.size != sizeof(nested_payload))
    ci_memory_test_fail("VFS implicit directory stat");

  vfs_dir_t root_dir = {0};
  if (!vfs_dir_open(&root_dir, "/"))
    ci_memory_test_fail("VFS root directory open");
  char entry_name[VFS_NAME_MAX];
  vfs_stat_t entry_stat;
  int saw_docs = 0;
  int dir_result = 0;
  while ((dir_result = vfs_dir_read(&root_dir, entry_name,
                                    sizeof(entry_name), &entry_stat)) > 0) {
    if (entry_name[0] == 'd' && entry_name[1] == 'o' &&
        entry_name[2] == 'c' && entry_name[3] == 's' &&
        entry_name[4] == '\0' &&
        entry_stat.type == VFS_NODE_DIRECTORY)
      saw_docs = 1;
  }
  if (dir_result < 0 || !saw_docs || !vfs_dir_close(&root_dir))
    ci_memory_test_fail("VFS root directory iteration");

  vfs_dir_t docs_dir = {0};
  if (!vfs_dir_open(&docs_dir, "/docs") ||
      vfs_dir_read(&docs_dir, entry_name, sizeof(entry_name), &entry_stat) != 1 ||
      entry_stat.type != VFS_NODE_FILE ||
      entry_name[0] != 'r' || entry_name[1] != 'e' ||
      entry_name[2] != 'a' || entry_name[3] != 'd' ||
      entry_name[4] != 'm' || entry_name[5] != 'e' ||
      entry_name[6] != '.' || entry_name[7] != 't' ||
      entry_name[8] != 'x' || entry_name[9] != 't' ||
      entry_name[10] != '\0' ||
      vfs_dir_read(&docs_dir, entry_name, sizeof(entry_name), &entry_stat) != 0 ||
      !vfs_dir_close(&docs_dir))
    ci_memory_test_fail("VFS child directory enumeration");
  vfs_dir_t retry_dir = {0};
  if (!vfs_dir_open(&retry_dir, "/docs") ||
      vfs_dir_read(&retry_dir, entry_name, 2u, &entry_stat) != -1 ||
      vfs_dir_read(&retry_dir, entry_name, sizeof(entry_name), &entry_stat) != 1 ||
      entry_name[0] != 'r' || entry_name[1] != 'e' ||
      entry_name[2] != 'a' || entry_name[3] != 'd' ||
      entry_name[4] != 'm' || entry_name[5] != 'e' ||
      entry_name[6] != '.' || entry_name[7] != 't' ||
      entry_name[8] != 'x' || entry_name[9] != 't' ||
      entry_name[10] != '\0' || !vfs_dir_close(&retry_dir))
    ci_memory_test_fail("VFS directory buffer retry");
  serial_print("[CI] VFS directory buffer retry passed\n");

  serial_print("[CI] VFS hierarchical paths and directory iteration passed\n");

  vfs_file_t nested_file_a = {0};
  vfs_file_t nested_file_b = {0};
  if (!vfs_open(&nested_file_a, "/docs/readme.txt") ||
      !vfs_open(&nested_file_b, "docs/readme.txt"))
    ci_memory_test_fail("VFS reference-counted repeated open");
  if (vfs_unlink("/docs/readme.txt") ||
      vfs_write("/docs/readme.txt", nested_payload, sizeof(nested_payload)))
    ci_memory_test_fail("VFS mutation rejected while file is open");
  if (!vfs_close(&nested_file_b) || vfs_unlink("/docs/readme.txt"))
    ci_memory_test_fail("VFS unlink blocked while one reference remains");
  if (!vfs_close(&nested_file_a) || !vfs_unlink("/docs/readme.txt"))
    ci_memory_test_fail("VFS close releases final node and permits unlink");
  if (vfs_stat("/docs/readme.txt", &vfs_info) ||
      vfs_stat("/docs", &directory_stat))
    ci_memory_test_fail("VFS unlink removes final implicit directory");

  /* SFS2 stores parent directory inodes; unlink must prune an empty chain. */
  if (!vfs_write("/prune/a/b/leaf.txt", nested_payload,
                 sizeof(nested_payload)) ||
      !vfs_stat("/prune", &directory_stat) ||
      !vfs_stat("/prune/a", &directory_stat) ||
      !vfs_stat("/prune/a/b", &directory_stat) ||
      !vfs_unlink("/prune/a/b/leaf.txt") ||
      vfs_stat("/prune", &directory_stat) ||
      vfs_stat("/prune/a", &directory_stat) ||
      vfs_stat("/prune/a/b", &directory_stat))
    ci_memory_test_fail("VFS unlink recursively prunes empty SFS2 directories");

  if (!vfs_create("/docs/empty.txt") ||
      !vfs_stat("/docs/empty.txt", &vfs_info) ||
      vfs_info.type != VFS_NODE_FILE || vfs_info.size != 0u)
    ci_memory_test_fail("VFS create empty file");
  vfs_file_t empty_file = {0};
  if (!vfs_open(&empty_file, "/docs/empty.txt") ||
      vfs_unlink("/docs/empty.txt") || !vfs_close(&empty_file) ||
      !vfs_unlink("/docs/empty.txt"))
    ci_memory_test_fail("VFS unlink and open-file lifetime");
  if (vfs_write("/ci.txt/child", "x", 2u))
    ci_memory_test_fail("VFS rejects file as parent directory");
  if (!vfs_write("/tree/leaf", nested_payload, sizeof(nested_payload)) ||
      vfs_write("/tree", "x", 2u) || vfs_create("/tree") ||
      vfs_write("/tree/leaf/child", "x", 2u) ||
      !vfs_unlink("/tree/leaf") || vfs_stat("/tree", &directory_stat))
    ci_memory_test_fail("VFS namespace parent/child type consistency");
  if (vfs_create("/"))
    ci_memory_test_fail("VFS rejects creating root directory as file");
  serial_print("[CI] VFS create/unlink and open-node reference rules passed\n");

  serial_print("[CI] VFS root mount/path/stat/read/write/ranged-read passed\n");

  // Unknown syscall numbers and invalid user pointers must fail safely.
  frame.rax = 0xFFFFFFFFu;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if ((int64_t)frame.rax != -1)
    ci_memory_test_fail("syscall unknown-number rejection");

  frame = (struct InterruptFrame){0};
  frame.rax = 1;
  frame.rdi = 0x12345u;
  frame.rsi = 4;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if ((int64_t)frame.rax != -1)
    ci_memory_test_fail("syscall write rejected invalid user pointer");

  frame = (struct InterruptFrame){0};
  frame.rax = 2;
  frame.rsi = 0;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != UINT64_MAX)
    ci_memory_test_fail("syscall mmap accepted zero length");

  frame = (struct InterruptFrame){0};
  frame.rax = 2;
  frame.rsi = 16u * 1024u * 1024u + 1u;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != UINT64_MAX)
    ci_memory_test_fail("syscall mmap accepted oversized length");

  frame = (struct InterruptFrame){0};
  frame.rax = 2;
  frame.rdi = 0x1000u;
  frame.rsi = PAGE_SIZE;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != UINT64_MAX)
    ci_memory_test_fail("syscall mmap accepted out-of-range requested address");

  // A two-page mapping must be user-accessible, writable, and zero-filled.
  frame = (struct InterruptFrame){0};
  frame.rax = 2;
  frame.rsi = PAGE_SIZE + 1u;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  void *user_buf = (void *)(uintptr_t)frame.rax;
  if (!user_buf || frame.rax == UINT64_MAX ||
      !VMM_IsUserRange(user_buf, PAGE_SIZE + 1u, 1))
    ci_memory_test_fail("syscall mmap failed valid two-page allocation");

  volatile uint8_t *bytes = (volatile uint8_t *)user_buf;
  volatile uint8_t *last_page = (volatile uint8_t *)((uintptr_t)user_buf + PAGE_SIZE);
  if (bytes[0] != 0 || bytes[PAGE_SIZE - 1u] != 0 || last_page[0] != 0)
    ci_memory_test_fail("syscall mmap did not zero mapped pages");

  bytes[0] = 'C';
  bytes[1] = 'I';
  bytes[2] = '\n';
  frame = (struct InterruptFrame){0};
  frame.rax = 1;
  frame.rdi = (uint64_t)(uintptr_t)user_buf;
  frame.rsi = 3;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != 3)
    ci_memory_test_fail("syscall write failed valid user buffer");

  // Open/read/seek/close a real file through the syscall ABI.
  const uint32_t expected_file_len = 700u;
  uint32_t mounted_file_size = 0;
  vfs_stat_t mounted_stat;
  if (!vfs_stat("/ci.txt", &mounted_stat) ||
      mounted_stat.type != VFS_NODE_FILE ||
      (mounted_file_size = mounted_stat.size) != expected_file_len)
    ci_memory_test_fail("VFS fixture file metadata");

  char *user_name = (char *)user_buf + 16u;
  static const char file_name[] = "/ci.txt";
  for (uint32_t i = 0; i < sizeof(file_name); i++)
    user_name[i] = file_name[i];
  char *user_read = (char *)user_buf + 128u;

  frame = (struct InterruptFrame){0};
  frame.rax = 3;
  frame.rdi = (uint64_t)(uintptr_t)user_name;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  int test_fd = (int)frame.rax;
  if (test_fd < 3)
    ci_memory_test_fail("syscall open valid VFS file");

  frame = (struct InterruptFrame){0};
  frame.rax = 3;
  frame.rdi = (uint64_t)(uintptr_t)user_name;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  int test_fd2 = (int)frame.rax;
  if (test_fd2 < 3 || test_fd2 == test_fd)
    ci_memory_test_fail("VFS independent second open");

  frame = (struct InterruptFrame){0};
  frame.rax = 4;
  frame.rdi = (uint64_t)test_fd;
  frame.rsi = (uint64_t)(uintptr_t)user_read;
  frame.rdx = expected_file_len;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != expected_file_len)
    ci_memory_test_fail("syscall read valid SimpleFS file");
  for (uint32_t i = 0; i < expected_file_len; i++) {
    uint8_t expected = (uint8_t)((i * 37u + 11u) & 0xFFu);
    if ((uint8_t)user_read[i] != expected)
      ci_memory_test_fail("syscall multi-sector file content mismatch");
  }

  /* Two independent opens of one file must maintain independent offsets. */
  frame = (struct InterruptFrame){0};
  frame.rax = 6;
  frame.rdi = (uint64_t)test_fd;
  frame.rsi = 0;
  frame.rdx = 0;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != 0)
    ci_memory_test_fail("VFS seek first independent handle");

  frame = (struct InterruptFrame){0};
  frame.rax = 4;
  frame.rdi = (uint64_t)test_fd;
  frame.rsi = (uint64_t)(uintptr_t)(user_read + 10u);
  frame.rdx = 1;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != 1)
    ci_memory_test_fail("VFS first independent handle read");

  frame = (struct InterruptFrame){0};
  frame.rax = 4;
  frame.rdi = (uint64_t)test_fd2;
  frame.rsi = (uint64_t)(uintptr_t)(user_read + 11u);
  frame.rdx = 2;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != 2 ||
      (uint8_t)user_read[10] != 11u ||
      (uint8_t)user_read[11] != 11u ||
      (uint8_t)user_read[12] != 48u)
    ci_memory_test_fail("VFS independent open-file offsets");

  frame = (struct InterruptFrame){0};
  frame.rax = 5;
  frame.rdi = (uint64_t)test_fd2;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != 0)
    ci_memory_test_fail("VFS close second independent handle");

  serial_print("[CI] VFS independent open-file handles and descriptor offsets passed\n");

  frame = (struct InterruptFrame){0};
  frame.rax = 6;
  frame.rdi = (uint64_t)test_fd;
  frame.rsi = 0;
  frame.rdx = 0;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != 0)
    ci_memory_test_fail("syscall seek to beginning");

  frame = (struct InterruptFrame){0};
  frame.rax = 6;
  frame.rdi = (uint64_t)test_fd;
  frame.rsi = 0;
  frame.rdx = 2;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != expected_file_len)
    ci_memory_test_fail("syscall seek to end");

  frame = (struct InterruptFrame){0};
  frame.rax = 5;
  frame.rdi = (uint64_t)test_fd;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != 0)
    ci_memory_test_fail("syscall close valid SimpleFS file");

  frame = (struct InterruptFrame){0};
  frame.rax = 4;
  frame.rdi = (uint64_t)test_fd;
  frame.rsi = (uint64_t)(uintptr_t)user_read;
  frame.rdx = 1;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if ((int64_t)frame.rax != -1)
    ci_memory_test_fail("syscall read succeeded after close");

  // Exercise SFS writes, metadata updates, and remount persistence.
  uint8_t write_data[700];
  const uint32_t write_len = (uint32_t)sizeof(write_data);
  for (uint32_t i = 0; i < write_len; i++)
    write_data[i] = (uint8_t)((i * 19u + 7u) & 0xFFu);
  if (!simplefs_write_file("ci-out.txt", write_data, write_len))
    ci_memory_test_fail("SimpleFS multi-sector write file");
  char verify_data[768];
  uint32_t verify_len = 0;
  if (!simplefs_read_file("ci-out.txt", verify_data, sizeof(verify_data),
                          &verify_len) || verify_len != write_len)
    ci_memory_test_fail("SimpleFS read-after-write");
  for (uint32_t i = 0; i < write_len; i++) {
    if ((uint8_t)verify_data[i] != write_data[i])
      ci_memory_test_fail("SimpleFS multi-sector write content mismatch");
  }
  if (!simplefs_init())
    ci_memory_test_fail("SimpleFS remount after write");
  verify_len = 0;
  if (!simplefs_read_file("ci-out.txt", verify_data, sizeof(verify_data),
                          &verify_len) || verify_len != write_len)
    ci_memory_test_fail("SimpleFS remount read");
  for (uint32_t i = 0; i < write_len; i++) {
    if ((uint8_t)verify_data[i] != write_data[i])
      ci_memory_test_fail("SimpleFS remount content mismatch");
  }

  // Zero-length files must still open and close cleanly through syscalls.
  if (!simplefs_write_file("empty.txt", "", 0))
    ci_memory_test_fail("SimpleFS empty-file write");
  if (!simplefs_init())
    ci_memory_test_fail("SimpleFS empty-file remount");
  uint32_t empty_size = 1;
  if (!simplefs_file_size("empty.txt", &empty_size) || empty_size != 0)
    ci_memory_test_fail("SimpleFS empty-file metadata");
  static const char empty_name[] = "empty.txt";
  for (uint32_t i = 0; i < sizeof(empty_name); i++)
    user_name[i] = empty_name[i];
  frame = (struct InterruptFrame){0};
  frame.rax = 3;
  frame.rdi = (uint64_t)(uintptr_t)user_name;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  int empty_fd = (int)frame.rax;
  if (empty_fd < 3)
    ci_memory_test_fail("syscall open empty file");
  frame = (struct InterruptFrame){0};
  frame.rax = 4;
  frame.rdi = (uint64_t)empty_fd;
  frame.rsi = (uint64_t)(uintptr_t)user_read;
  frame.rdx = 1;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != 0)
    ci_memory_test_fail("syscall read empty file");
  frame = (struct InterruptFrame){0};
  frame.rax = 5;
  frame.rdi = (uint64_t)empty_fd;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != 0)
    ci_memory_test_fail("syscall close empty file");

  frame = (struct InterruptFrame){0};
  frame.rax = 4;
  frame.rdi = 2;
  frame.rsi = (uint64_t)(uintptr_t)user_buf;
  frame.rdx = 3;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if ((int64_t)frame.rax != -1)
    ci_memory_test_fail("syscall read accepted invalid file descriptor");

  frame = (struct InterruptFrame){0};
  frame.rax = 5;
  frame.rdi = 2;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if ((int64_t)frame.rax != -1)
    ci_memory_test_fail("syscall close accepted invalid file descriptor");

  frame = (struct InterruptFrame){0};
  frame.rax = 6;
  frame.rdi = 2;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if ((int64_t)frame.rax != -1)
    ci_memory_test_fail("syscall seek accepted invalid file descriptor");

  frame = (struct InterruptFrame){0};
  frame.rax = 9;
  frame.rsi = 0;
  frame.rdx = 1;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != UINT64_MAX)
    ci_memory_test_fail("syscall framebuffer rejected invalid-dimension test");

  if (!VMM_UnmapPage(user_buf, 1) ||
      !VMM_UnmapPage((void *)((uintptr_t)user_buf + PAGE_SIZE), 1))
    ci_memory_test_fail("syscall mmap teardown");
  if (VMM_IsUserRange(user_buf, 1, 0))
    ci_memory_test_fail("syscall mmap left stale user mapping");

  serial_print("[CI] SimpleFS multi-sector/empty-file persistence and syscall file I/O passed\n");
}

static void ci_framebuffer_syscall_self_test(void) {
  struct InterruptFrame frame = {0};

  /* Syscall 9 targets the Doom window's off-screen surface. Open that
     surface first; it intentionally does not draw directly into framebuffer[0]. */
  frame.rax = 11;
  syscall_handler((uint64_t)(uintptr_t)&frame);
  if (frame.rax != 0)
    ci_memory_test_fail("framebuffer test could not open Doom surface");

  frame = (struct InterruptFrame){0};
  frame.rax = 2;
  frame.rsi = sizeof(uint32_t);
  syscall_handler((uint64_t)(uintptr_t)&frame);

  void *user_pixel = (void *)(uintptr_t)frame.rax;
  if (!user_pixel || frame.rax == UINT64_MAX ||
      !VMM_IsUserRange(user_pixel, sizeof(uint32_t), 1))
    ci_memory_test_fail("framebuffer test mmap");

  const uint32_t expected = 0x0055AA33u;
  *(volatile uint32_t *)user_pixel = expected;

  frame = (struct InterruptFrame){0};
  frame.rax = 9;
  frame.rdi = (uint64_t)(uintptr_t)user_pixel;
  frame.rsi = 1;
  frame.rdx = 1;
  syscall_handler((uint64_t)(uintptr_t)&frame);

  if (frame.rax != 0)
    ci_memory_test_fail("framebuffer syscall rejected active Doom surface");

  if (!VMM_UnmapPage(user_pixel, 1) ||
      VMM_IsUserRange(user_pixel, sizeof(uint32_t), 0))
    ci_memory_test_fail("framebuffer test mmap teardown");

  serial_print("[CI] Successful framebuffer syscall draw passed\n");
  extern void desktop_doom_ci_test_cleanup(void);
  desktop_doom_ci_test_cleanup();
  serial_print("[CI] Doom test surface cleanup passed\n");
}

#endif


void kernel_main(BootInfo *bootInfo) {
  uint64_t val;
  serial_print("[KERNEL] Entered kernel_main\n");
  serial_print("[VexOS] Version " VEXOS_VERSION " | build " VEXOS_BUILD_ID "\n");
  serial_print("[KERNEL] BUILD: xhci-portscan-v2\n");
  // ... Console, PMM, VMM init ...
  serial_print("[KERNEL] Calling ConsoleInit...\n");
  ConsoleInit(bootInfo);
  g_ConsoleReady = 1;

  serial_print("[KERNEL] Clearing Framebuffer...\n");
  uint32_t *fb = bootInfo->framebuffer;
  for (uint32_t i = 0; i < bootInfo->width * bootInfo->height; i++) {
    fb[i] = 0x001122;
  }

  PrintString("Tiny64 Kernel Loaded!\n", 0xFFFFFF);
  PrintString("VexOS " VEXOS_VERSION " (" VEXOS_BUILD_ID ")\n", 0xFFFFFF);
  serial_print("[KERNEL] Setting up GDT...\n");
  SetupGDT();
  // Setup TSS/IST so hardware IRQs can run on a dedicated interrupt stack.
  TSS_Init(0);
  serial_print("[KERNEL] Setting up IDT...\n");
  SetupIDT();

  // PMM
  serial_print("[KERNEL] Initializing PMM...\n");
  void *bitmap_addr = (void *)bootInfo->LargestFreeRegion.Base;
  uint64_t mem_size = bootInfo->LargestFreeRegion.Size;

  hw_profile_detect_early(mem_size, bootInfo->width, bootInfo->height,
                          bootInfo->pitch);

  serial_print("[KERNEL] PMM Region Base: ");
  val = (uint64_t)bitmap_addr;
  for (int i = 15; i >= 0; i--) {
    char c = (val >> (i * 4)) & 0xF;
    if (c < 10)
      c += '0';
    else
      c += 'A' - 10;
    char s[2] = {c, 0};
    serial_print(s);
  }
  serial_print("\n");

  serial_print("[KERNEL] PMM Region Size: ");
  val = mem_size;
  for (int i = 15; i >= 0; i--) {
    char c = (val >> (i * 4)) & 0xF;
    if (c < 10)
      c += '0';
    else
      c += 'A' - 10;
    char s[2] = {c, 0};
    serial_print(s);
  }
  serial_print("\n");

  PMM_Init(mem_size, bitmap_addr);

  uint64_t total_pages = mem_size / 4096ull;
  uint64_t bitmap_bytes = (total_pages + 7ull) / 8ull;
  uint64_t bitmap_pages = (bitmap_bytes + 4095ull) / 4096ull;
  if (bitmap_pages == 0)
    bitmap_pages = 1;
  if (bitmap_pages >= total_pages) {
    serial_print("[KERNEL] FATAL: PMM region is too small for its bitmap\n");
    while (1) { __asm__ volatile("cli; hlt"); }
  }
  serial_print("[KERNEL] Reserving ");
  {
    char s[32];
    int len = 0;
    uint64_t tmp = bitmap_pages;
    if (tmp == 0)
      s[len++] = '0';
    else {
      char buf[32];
      int i = 0;
      while (tmp > 0) {
        buf[i++] = (tmp % 10) + '0';
        tmp /= 10;
      }
      while (i > 0)
        s[len++] = buf[--i];
    }
    s[len] = 0;
    serial_print(s);
  }
  serial_print(" pages for bitmap.\n");

  PMM_FreePages(
      (void *)(bootInfo->LargestFreeRegion.Base + (bitmap_pages * 4096)),
      total_pages - bitmap_pages);
  PrintString("PMM Initialized.\n", 0x00FF00);

  // VMM
  serial_print("[KERNEL] Initializing VMM...\n");
  if (!VMM_Init()) {
    serial_print("[KERNEL] FATAL: VMM root allocation failed\n");
    while (1) { __asm__ volatile("cli; hlt"); }
  }
  // Identity map first 128MB
  serial_print("[KERNEL] Mapping first 128MB...\n");
  for (uint64_t i = 0; i < 0x8000000; i += 4096)
    vmm_map_or_halt((void *)i, (void *)i, PAGE_WRITE);

  // Identity map the PMM region itself so we can access it
  serial_print("[KERNEL] Mapping PMM Region...\n");
  uint64_t pmm_map_bytes = mem_size;
  if (pmm_map_bytes > (256ull * 1024ull * 1024ull)) {
    pmm_map_bytes = (256ull * 1024ull * 1024ull);
  }
  for (uint64_t i = 0; i < pmm_map_bytes; i += 4096) {
    uint64_t addr = bootInfo->LargestFreeRegion.Base + i;
    vmm_map_or_halt((void *)addr, (void *)addr, PAGE_WRITE);
  }

  uint64_t fb_base = (uint64_t)bootInfo->framebuffer;
  uint64_t fb_size = (uint64_t)bootInfo->height * bootInfo->pitch * 4ull;
  if (pci_has_qemu_stdvga()) {
    /* DISPI reports video memory in 64 KiB units. Only identity-map a second
       page when the adapter reports enough real VRAM to hold both pages. */
    uint16_t video_memory_64k = qemu_vbe_read(0x0Au);
    uint64_t video_memory_bytes = (uint64_t)video_memory_64k * 65536ull;
    if (fb_size <= UINT64_MAX / 2ull &&
        video_memory_64k != 0u && video_memory_64k != 0xFFFFu &&
        fb_size * 2ull <= video_memory_bytes) {
      fb_size *= 2ull;
      serial_print("[KERNEL] QEMU stdvga detected; mapping two framebuffer pages\n");
    } else {
      serial_print("[KERNEL] QEMU stdvga detected; one framebuffer page fits VRAM\n");
    }
  }
  serial_print("[KERNEL] Mapping Framebuffer...\n");
  for (uint64_t i = 0; i < fb_size; i += 4096)
    vmm_map_or_halt((void *)(fb_base + i), (void *)(fb_base + i), PAGE_WRITE);

  uint64_t rip = 0;
  __asm__ volatile("lea (%%rip), %0" : "=r"(rip));
  uint64_t stack_addr_pre = (uint64_t)&val;

  uint64_t rip_start = (rip > 0x800000) ? (rip - 0x800000) : 0;
  rip_start &= ~0xFFFULL;
  uint64_t rip_end = (rip + 0x800000) & ~0xFFFULL;
  for (uint64_t a = rip_start; a <= rip_end; a += 4096) {
    vmm_map_or_halt((void *)a, (void *)a, PAGE_WRITE);
  }

  uint64_t sp_start =
      (stack_addr_pre > 0x200000) ? (stack_addr_pre - 0x200000) : 0;
  sp_start &= ~0xFFFULL;
  uint64_t sp_end = (stack_addr_pre + 0x200000) & ~0xFFFULL;
  for (uint64_t a = sp_start; a <= sp_end; a += 4096) {
    vmm_map_or_halt((void *)a, (void *)a, PAGE_WRITE);
  }

  serial_print("[KERNEL] Activating VMM...\n");
  VMM_Activate();
  PrintString("VMM Initialized.\n", 0x00FF00);

  // Stack Check
  uint64_t stack_addr = (uint64_t)&val; // val is on the stack
  serial_print("[KERNEL] Stack Address: ");
  for (int i = 15; i >= 0; i--) {
    char c = (stack_addr >> (i * 4)) & 0xF;
    if (c < 10)
      c += '0';
    else
      c += 'A' - 10;
    char s[2] = {c, 0};
    serial_print(s);
  }
  serial_print("\n");

  // Heap
  serial_print("[KERNEL] Initializing Heap...\n");
  uint64_t fb_bytes =
      (uint64_t)bootInfo->pitch * (uint64_t)bootInfo->height * 4u;
  uint64_t slack_bytes = hw_tuning_get()->heap_slack_bytes;
  // The desktop owns two full framebuffer buffers (background + backbuffer).
  // Keep additional working room for decoded image data and filesystem reads.
  uint64_t heap_bytes = (2ull * fb_bytes) + slack_bytes;
  if (heap_bytes < (24ull * 1024ull * 1024ull)) {
    heap_bytes = (24ull * 1024ull * 1024ull);
  }
  uint64_t heap_pages = (heap_bytes + 4095ull) / 4096ull;
  if (heap_pages < 4) {
    heap_pages = 4;
  }

  void *heap_start = PMM_AllocatePage();

  serial_print("[KERNEL] Heap Start: ");
  uint64_t hval = (uint64_t)heap_start;
  for (int i = 15; i >= 0; i--) {
    char c = (hval >> (i * 4)) & 0xF;
    if (c < 10)
      c += '0';
    else
      c += 'A' - 10;
    char s[2] = {c, 0};
    serial_print(s);
  }
  serial_print("\n");

  if (heap_start == NULL) {
    serial_print("[KERNEL] FATAL: PMM_AllocatePage returned NULL\n");
    while (1)
      ;
  }

  // Allocate and map remaining heap pages, requiring contiguity.
  uint64_t heap_base = (uint64_t)heap_start;
  vmm_map_or_halt((void *)heap_base, (void *)heap_base, PAGE_WRITE);
  for (uint64_t p = 1; p < heap_pages; p++) {
    void *next = PMM_AllocatePage();
    if (next == NULL) {
      serial_print(
          "[KERNEL] FATAL: PMM_AllocatePage out of memory during heap grow\n");
      while (1)
        ;
    }
    uint64_t want = heap_base + (p * 4096ull);
    if ((uint64_t)next != want) {
      serial_print("[KERNEL] FATAL: heap pages not contiguous\n");
      while (1)
        ;
    }
    vmm_map_or_halt((void *)want, (void *)want, PAGE_WRITE);
  }

  Heap_Init((void *)heap_base, (size_t)(heap_pages * 4096ull));

  if (FontEngine_Init()) {
    serial_print("[FONT] stb_truetype runtime initialized (6 families, regular/bold)\n");
#ifdef VEXOS_CI_INPUT_TEST
    if (!FontEngine_SelfTest())
      ci_memory_test_fail("TrueType font loading, metrics, or glyph rasterization");
    serial_print("[CI] TrueType font loading, metrics, and glyph rasterization passed\n");
#endif
  } else {
    serial_print("[FONT] TrueType initialization failed; retaining CGA fallback\n");
#ifdef VEXOS_CI_INPUT_TEST
    ci_memory_test_fail("TrueType font engine initialization");
#endif
  }
#ifdef VEXOS_CI_INPUT_TEST
  ci_memory_self_test();
  ci_address_space_self_test();
  if (!usermode_ci_self_test())
    ci_memory_test_fail("ELF validation/rollback self-test");
  serial_print("[CI] ELF header/segment validation and rollback passed\n");
  if (!Task_CI_SelfTest())
    ci_memory_test_fail("task frame/scheduler self-test");
  serial_print("[CI] Task IRET frame and scheduler checks passed\n");
  ci_syscall_self_test();
  ci_install_ring3_elf_fixture();
#endif
  serial_print("[KERNEL] Heap Initialized Successfully.\n");
  PrintString("Heap Initialized.\n", 0x00FF00);

  // Network devices share the normal PCI discovery path. Start the
  // device registry only after the kernel heap and VMM are ready.
  netdev_init();
  ethernet_init();
  if (!arp_init())
    serial_print("[ARP] Failed to register ARP protocol handler\n");
  netdev_set_receive_handler(ethernet_receive);
  serial_print("[NET] Network-device registry, Ethernet II dispatcher, and ARP initialized\n");

  // PCI Enumeration (must happen before wizard for xHCI input)
  serial_print("[KERNEL] Starting PCI Enumeration...\n");
  PrintString("Scanning PCI Bus...\n", 0xFFFFFF);
  pci_enumerate();
#ifdef VEXOS_CI_INPUT_TEST
  ci_ahci_self_test();
#endif
  hw_profile_update_pci_gpu();
  serial_print("[KERNEL] PCI enumeration returned\n");

#ifdef VEXOS_CI_SKIP_WIZARD
  // CI-only path: bypass the interactive first-boot wizard for automated QEMU input.
  serial_print("[CI] Setup wizard skipped for automated desktop test\n");
#else
  // Run fullscreen setup wizard on first boot (now with working xHCI input)
  serial_print("[KERNEL] Starting Fullscreen Setup Wizard...\n");
  desktop_run_fullscreen_wizard(bootInfo);
#endif

  // Initialize desktop after setup (or the CI-only wizard bypass).
  serial_print("[KERNEL] Setup complete, initializing desktop...\n");
  desktop_init(bootInfo);

  // Keep subsequent diagnostics on COM1 only; do not paint boot text over
  // the newly initialized desktop and taskbar.
  g_ConsoleReady = 0;
  serial_print("[KERNEL] Desktop initialized\n");

#ifdef VEXOS_CI_INPUT_TEST
  // Run only after desktop_init has installed the framebuffer target.
  ci_framebuffer_syscall_self_test();
#endif

  Task_Init();
  Process_Init();
#ifdef VEXOS_CI_INPUT_TEST
  if (!Process_CI_SelfTest())
    ci_memory_test_fail("process manager lifecycle self-test");
  serial_print("[CI] Process manager creation, scheduler CR3/TSS switching, and reaping passed\n");
  extern void Task_CI_StartRuntimeTest(void);
  extern int Task_CI_RuntimeTestPassed(void);
  extern uint64_t Task_CI_RuntimeBadFrameCS(void);
  Task_CI_StartRuntimeTest();
#endif
  serial_print("[KERNEL] Task_Init done\n");

  // Setup Timer
  // TODO: PIC bring-up. We currently avoid touching legacy PIC IMR ports
  // because on some firmware/QEMU configs it can hang during early boot.
  // Keep the PIT initialization for later, but do not rely on Timer_Ticks.
  __asm__ volatile("cli");

  serial_print("[KERNEL] PIT_Init(1000)...\n");
  PIT_Init(1000); // 1000 Hz
  serial_print("[KERNEL] PIT_Init done\n");

  // Enable IRQ0 timer interrupts so ring3 code that loops forever still gets
  // preempted and the OS remains responsive.
  serial_print("[KERNEL] PIC_Remap...\n");
  PIC_Remap();
  // Mask all IRQs except IRQ0. Our IDT only installs an IRQ0 handler right now;
  // any other unmasked/pending IRQ would vector into an uninitialized IDT
  // entry and fault immediately after sti.
  for (uint8_t irq = 1; irq < 16; irq++) {
    PIC_SetMask(irq);
  }
  serial_print("[KERNEL] PIC_ClearMask(0)...\n");
  PIC_ClearMask(0);
  serial_print("[KERNEL] sti\n");
  __asm__ volatile("sti");

  if (g_ConsoleReady) {
    PrintString("Starting Preemptive Multitasking...\n", 0xFFFFFF);
  }
  serial_print("[KERNEL] Starting desktop loop...\n");

  // Main loop - precise time-based frame rate limiter
  uint32_t target_fps = hw_tuning_get()->target_fps;
  if (target_fps == 0) {
    target_fps = 60;
  }
  uint32_t frame_delay = 1000 / target_fps;
  uint64_t last_frame_ticks = Timer_Ticks();

  int ci_task_runtime_reported = 0;
#ifdef VEXOS_CI_INPUT_TEST
  int ci_ring3_stress_reported = 0;
#endif
  while (1) {
#ifdef VEXOS_CI_INPUT_TEST
    if (!ci_task_runtime_reported && Task_CI_RuntimeTestPassed()) {
      serial_print("[CI] IRQ0 preempted and executed both runtime tasks\n");
      ci_task_runtime_reported = 1;
    }
    if (!ci_task_runtime_reported && Task_CI_RuntimeBadFrameCS() != 0) {
      serial_print("[CI] IRQ scheduler rejected an invalid task CS frame\n");
      ci_task_runtime_reported = 1;
    }
    if (!ci_ring3_stress_reported && Task_CI_RuntimeTestPassed()) {
      if (!Process_CI_UserStressTest())
        ci_memory_test_fail("concurrent ring-3 process stress test");
      serial_print("[CI] Concurrent ring-3 workers, private mmap state, memory isolation, and exit/reap passed\n");
      ci_ring3_stress_reported = 1;
    }
#endif
    // Poll network rings in task context; this first driver does not enable
    // device interrupts until VexOS has general IRQ routing.
    netdev_poll_all(16u);
    arp_poll();

    // Keep xHCI polling frequent. The poll is atomic against PIT task switches.
    xhci_poll_events_atomic();

    // Run desktop tick
    desktop_tick();

    // Yield/Wait to maintain targeted FPS
    uint64_t current_ticks = Timer_Ticks();
    uint64_t elapsed = current_ticks - last_frame_ticks;
    if (elapsed < frame_delay) {
      uint64_t wait_ticks = frame_delay - elapsed;
      uint64_t target_ticks = current_ticks + wait_ticks;
      while (Timer_Ticks() < target_ticks) {
        // IRQ0 advances Timer_Ticks; halt until the next tick instead of
        // burning a CPU core in a polling loop. STI;HLT avoids the check/HLT
        // lost-wakeup window when the timer fires between the condition and HLT.
        __asm__ volatile("sti; hlt" ::: "memory");
        // Service USB reports and keyboard input on every PIT wakeup, not
        // only once per rendered frame. This keeps input latency near 1 ms
        // while the desktop renderer remains frame-rate limited.
        xhci_poll_events_atomic();
        netdev_poll_all(16u);
        arp_poll();
        // Desktop rendering stays on the frame cadence below. Calling
        // desktop_tick() here used to repaint the full scene on every PIT
        // wakeup (up to 1000 times/second), starving window interaction.
      }
    }
    last_frame_ticks = Timer_Ticks();
  }
}
