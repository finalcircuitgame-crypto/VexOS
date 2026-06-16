#include "bootinfo.h"
#include "desktop.h"
#include "gdt.h"
#include "heap.h"
#include "hw_profile.h"
#include "idt.h"
#include "pmm.h"
#include "simplefs.h"
#include "task.h"
#include "vmm.h"
#include "pci.h"
#include <stddef.h>
#include <stdint.h>

// Defined in other files
void ConsoleInit(BootInfo *bootInfo);
void PrintString(const char *str, uint32_t color);
void SetupGDT();
void SetupIDT();
void PMM_FreePages(void *addr, uint64_t count);
void xhci_poll_events();

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
  // Always print to COM1
  const char *p = str;
  while (*p) {
    outb(0x3F8, *p++);
  }

  // Also print to framebuffer console once initialized
  if (g_ConsoleReady) {
    PrintString(str, 0xFFFFFF);
  }
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

void kernel_main(BootInfo *bootInfo) {
  uint64_t val;
  serial_print("[KERNEL] Entered kernel_main\n");
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

  uint64_t bitmap_pages = ((mem_size / 4096 / 8) + 4095) / 4096;
  if (bitmap_pages == 0)
    bitmap_pages = 1;
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
      (mem_size / 4096) - bitmap_pages);
  PrintString("PMM Initialized.\n", 0x00FF00);

  // VMM
  serial_print("[KERNEL] Initializing VMM...\n");
  VMM_Init();
  // Identity map first 128MB
  serial_print("[KERNEL] Mapping first 128MB...\n");
  for (uint64_t i = 0; i < 0x8000000; i += 4096)
    VMM_MapPage((void *)i, (void *)i, PAGE_WRITE);

  // Identity map the PMM region itself so we can access it
  serial_print("[KERNEL] Mapping PMM Region...\n");
  uint64_t pmm_map_bytes = mem_size;
  if (pmm_map_bytes > (256ull * 1024ull * 1024ull)) {
    pmm_map_bytes = (256ull * 1024ull * 1024ull);
  }
  for (uint64_t i = 0; i < pmm_map_bytes; i += 4096) {
    uint64_t addr = bootInfo->LargestFreeRegion.Base + i;
    VMM_MapPage((void *)addr, (void *)addr, PAGE_WRITE);
  }

  uint64_t fb_base = (uint64_t)bootInfo->framebuffer;
  uint64_t fb_size = (uint64_t)bootInfo->height * bootInfo->pitch * 4;
  serial_print("[KERNEL] Mapping Framebuffer...\n");
  for (uint64_t i = 0; i < fb_size; i += 4096)
    VMM_MapPage((void *)(fb_base + i), (void *)(fb_base + i), PAGE_WRITE);

  uint64_t rip = 0;
  __asm__ volatile("lea (%%rip), %0" : "=r"(rip));
  uint64_t stack_addr_pre = (uint64_t)&val;

  uint64_t rip_start = (rip > 0x800000) ? (rip - 0x800000) : 0;
  rip_start &= ~0xFFFULL;
  uint64_t rip_end = (rip + 0x800000) & ~0xFFFULL;
  for (uint64_t a = rip_start; a <= rip_end; a += 4096) {
    VMM_MapPage((void *)a, (void *)a, PAGE_WRITE);
  }

  uint64_t sp_start =
      (stack_addr_pre > 0x200000) ? (stack_addr_pre - 0x200000) : 0;
  sp_start &= ~0xFFFULL;
  uint64_t sp_end = (stack_addr_pre + 0x200000) & ~0xFFFULL;
  for (uint64_t a = sp_start; a <= sp_end; a += 4096) {
    VMM_MapPage((void *)a, (void *)a, PAGE_WRITE);
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
  uint64_t heap_bytes = fb_bytes + slack_bytes;
  if (heap_bytes < (8ull * 1024ull * 1024ull)) {
    heap_bytes = (8ull * 1024ull * 1024ull);
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
  VMM_MapPage((void *)heap_base, (void *)heap_base, PAGE_WRITE);
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
    VMM_MapPage((void *)want, (void *)want, PAGE_WRITE);
  }

  Heap_Init((void *)heap_base, (size_t)(heap_pages * 4096ull));
  serial_print("[KERNEL] Heap Initialized Successfully.\n");
  PrintString("Heap Initialized.\n", 0x00FF00);

  // PCI Enumeration (must happen before wizard for xHCI input)
  serial_print("[KERNEL] Starting PCI Enumeration...\n");
  PrintString("Scanning PCI Bus...\n", 0xFFFFFF);
  pci_enumerate();
  hw_profile_update_pci_gpu();
  serial_print("[KERNEL] PCI enumeration returned\n");

  // Run fullscreen setup wizard on first boot (now with working xHCI)
  serial_print("[KERNEL] Starting Fullscreen Setup Wizard...\n");
  desktop_run_fullscreen_wizard(bootInfo);

  // After setup completes, initialize desktop
  serial_print("[KERNEL] Setup complete, initializing desktop...\n");
  desktop_init(bootInfo);

  // Stop drawing serial/boot text to the framebuffer
  g_ConsoleReady = 0;

  Task_Init();
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
  serial_print("[KERNEL] Starting main loop (uncapped)...\n");

  // Main loop - precise time-based frame rate limiter
  uint32_t target_fps = hw_tuning_get()->target_fps;
  if (target_fps == 0) {
    target_fps = 60;
  }
  uint32_t frame_delay = 1000 / target_fps;
  uint64_t last_frame_ticks = Timer_Ticks();

  while (1) {
    // Keep xHCI polling frequent so mouse/keyboard remain responsive
    xhci_poll_events();

    // Run desktop tick
    desktop_tick();

    // Yield/Wait to maintain targeted FPS
    uint64_t current_ticks = Timer_Ticks();
    uint64_t elapsed = current_ticks - last_frame_ticks;
    if (elapsed < frame_delay) {
      uint64_t wait_ticks = frame_delay - elapsed;
      uint64_t target_ticks = current_ticks + wait_ticks;
      while (Timer_Ticks() < target_ticks) {
        __asm__ volatile("pause");
      }
    }
    last_frame_ticks = Timer_Ticks();
  }
}
