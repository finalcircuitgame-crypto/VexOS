#include "../include/bootinfo.h"
#include "../include/gdt.h"
#include "../include/idt.h"
#include "../include/pmm.h"
#include "../include/vmm.h"
#include "../include/heap.h"
#include "../include/task.h"
#include "pci.h"
#include <stddef.h>

// Defined in other files
void ConsoleInit(BootInfo *bootInfo);
void PrintString(const char *str, uint32_t color);
void SetupGDT();
void SetupIDT();
void PMM_FreePages(void* addr, uint64_t count);
void xhci_poll_events();

// Serial Port output
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
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

#include "../include/interrupts.h"


void PIC_Remap();

void taskA() {
    while(1) {
        PrintString(" [A] ", 0x00FFFF);
        serial_print("A");
        for(volatile int i=0; i<5000000; i++);
    }
}

void taskB() {
    while(1) {
        PrintString(" [B] ", 0xFFFF00);
        serial_print("B");
        for(volatile int i=0; i<5000000; i++);
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
    serial_print("[KERNEL] Setting up IDT...\n");
    SetupIDT();

    // PMM
    serial_print("[KERNEL] Initializing PMM...\n");
    void* bitmap_addr = (void*)bootInfo->LargestFreeRegion.Base;
    uint64_t mem_size = bootInfo->LargestFreeRegion.Size;
    
    serial_print("[KERNEL] PMM Region Base: ");
    val = (uint64_t)bitmap_addr;
    for (int i = 15; i >= 0; i--) {
        char c = (val >> (i * 4)) & 0xF;
        if (c < 10) c += '0'; else c += 'A' - 10;
        char s[2] = {c, 0};
        serial_print(s);
    }
    serial_print("\n");

    serial_print("[KERNEL] PMM Region Size: ");
    val = mem_size;
    for (int i = 15; i >= 0; i--) {
        char c = (val >> (i * 4)) & 0xF;
        if (c < 10) c += '0'; else c += 'A' - 10;
        char s[2] = {c, 0};
        serial_print(s);
    }
    serial_print("\n");

    PMM_Init(mem_size, bitmap_addr);

    uint64_t bitmap_pages = ((mem_size / 4096 / 8) + 4095) / 4096;
    if (bitmap_pages == 0) bitmap_pages = 1;
    serial_print("[KERNEL] Reserving ");
    {
        char s[32];
        int len = 0;
        uint64_t tmp = bitmap_pages;
        if (tmp == 0) s[len++] = '0';
        else {
            char buf[32];
            int i = 0;
            while (tmp > 0) { buf[i++] = (tmp % 10) + '0'; tmp /= 10; }
            while (i > 0) s[len++] = buf[--i];
        }
        s[len] = 0;
        serial_print(s);
    }
    serial_print(" pages for bitmap.\n");

    PMM_FreePages((void*)(bootInfo->LargestFreeRegion.Base + (bitmap_pages * 4096)), (mem_size / 4096) - bitmap_pages);
    PrintString("PMM Initialized.\n", 0x00FF00);

    // VMM
    serial_print("[KERNEL] Initializing VMM...\n");
    VMM_Init();
    // Identity map first 128MB
    serial_print("[KERNEL] Mapping first 128MB...\n");
    for (uint64_t i = 0; i < 0x8000000; i += 4096) VMM_MapPage((void*)i, (void*)i, PAGE_WRITE);
    
    // Identity map the PMM region itself so we can access it
    serial_print("[KERNEL] Mapping PMM Region...\n");
    for (uint64_t i = 0; i < 0x1000000; i += 4096) { // Map at least 16MB of the free region
        uint64_t addr = bootInfo->LargestFreeRegion.Base + i;
        VMM_MapPage((void*)addr, (void*)addr, PAGE_WRITE);
    }

    uint64_t fb_base = (uint64_t)bootInfo->framebuffer;
    uint64_t fb_size = (uint64_t)bootInfo->height * bootInfo->pitch * 4;
    serial_print("[KERNEL] Mapping Framebuffer...\n");
    for (uint64_t i = 0; i < fb_size; i += 4096) VMM_MapPage((void*)(fb_base + i), (void*)(fb_base + i), PAGE_WRITE);

    uint64_t rip = 0;
    __asm__ volatile ("lea (%%rip), %0" : "=r"(rip));
    uint64_t stack_addr_pre = (uint64_t)&val;

    uint64_t rip_start = (rip > 0x800000) ? (rip - 0x800000) : 0;
    rip_start &= ~0xFFFULL;
    uint64_t rip_end = (rip + 0x800000) & ~0xFFFULL;
    for (uint64_t a = rip_start; a <= rip_end; a += 4096) {
        VMM_MapPage((void*)a, (void*)a, PAGE_WRITE);
    }

    uint64_t sp_start = (stack_addr_pre > 0x200000) ? (stack_addr_pre - 0x200000) : 0;
    sp_start &= ~0xFFFULL;
    uint64_t sp_end = (stack_addr_pre + 0x200000) & ~0xFFFULL;
    for (uint64_t a = sp_start; a <= sp_end; a += 4096) {
        VMM_MapPage((void*)a, (void*)a, PAGE_WRITE);
    }
    
    serial_print("[KERNEL] Activating VMM...\n");
    VMM_Activate();
    PrintString("VMM Initialized.\n", 0x00FF00);

    // Stack Check
    uint64_t stack_addr = (uint64_t)&val; // val is on the stack
    serial_print("[KERNEL] Stack Address: ");
    for (int i = 15; i >= 0; i--) {
        char c = (stack_addr >> (i * 4)) & 0xF;
        if (c < 10) c += '0'; else c += 'A' - 10;
        char s[2] = {c, 0};
        serial_print(s);
    }
    serial_print("\n");

    // Heap
    serial_print("[KERNEL] Initializing Heap...\n");
    void* heap_start = PMM_AllocatePage();
    
    serial_print("[KERNEL] Heap Start: ");
    uint64_t hval = (uint64_t)heap_start;
    for (int i = 15; i >= 0; i--) {
        char c = (hval >> (i * 4)) & 0xF;
        if (c < 10) c += '0'; else c += 'A' - 10;
        char s[2] = {c, 0};
        serial_print(s);
    }
    serial_print("\n");

    if (heap_start == NULL) {
        serial_print("[KERNEL] FATAL: PMM_AllocatePage returned NULL\n");
        while(1);
    }

    // Map the heap page explicitly if needed (though it should be in identity map or PMM region)
    // Check if it's mapped?
    // For now, just try to map it to be safe
    VMM_MapPage(heap_start, heap_start, PAGE_WRITE);

    Heap_Init(heap_start, 4096);
    serial_print("[KERNEL] Heap Initialized Successfully.\n");
    PrintString("Heap Initialized.\n", 0x00FF00);

    // PCI Enumeration
    serial_print("[KERNEL] Starting PCI Enumeration...\n");
    PrintString("Scanning PCI Bus...\n", 0xFFFFFF);
    pci_enumerate();

    // Multitasking Setup
    Task_Init();
    void* stackA = kmalloc(4096);
    void* stackB = kmalloc(4096);
    Task_Create(taskA, (char*)stackA + 4096);
    Task_Create(taskB, (char*)stackB + 4096);
    
    // Setup Timer
    PIC_Remap();
    PIT_Init(100); // 100 Hz

    PrintString("Starting Preemptive Multitasking...\n", 0xFFFFFF);
    serial_print("[KERNEL] Starting Preemptive Multitaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaasking...\n");

    __asm__ volatile ("sti"); // Enable Interrupts

    while (1) {
        xhci_poll_events();
        for(volatile int i=0; i<200000; i++);
    }
}

