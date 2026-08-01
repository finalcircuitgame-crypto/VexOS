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

#include "../../include/heap.h"
#include "../../include/simplefs.h"
#include "../../include/vmm.h"
#include "../../include/pmm.h"
#include "../../include/input.h"

extern uint64_t Timer_Ticks(void);

#define MAX_OPEN_FILES 16
typedef struct {
    int used;
    void* data;
    uint32_t size;
    uint32_t cursor;
} open_file_t;

static open_file_t g_files[MAX_OPEN_FILES] = {0};

static int sys_open(const char* filename) {
    char name_buf[64];
    // Copy name safely
    for(int i=0; i<63; i++) {
        name_buf[i] = filename[i];
        if (name_buf[i] == 0) break;
    }
    name_buf[63] = 0;

    int fd = -1;
    for(int i=3; i<MAX_OPEN_FILES; i++) {
        if (!g_files[i].used) {
            fd = i;
            break;
        }
    }
    if (fd == -1) return -1;

    uint32_t size = 0;
    if (simplefs_file_size(name_buf, &size) != 0) return -1;

    void* data = kmalloc(size);
    if (!data) return -1;

    uint32_t out_len = 0;
    if (simplefs_read_file(name_buf, data, size, &out_len) != 0) {
        kfree(data);
        return -1;
    }

    g_files[fd].used = 1;
    g_files[fd].data = data;
    g_files[fd].size = size;
    g_files[fd].cursor = 0;
    return fd;
}

static int sys_read(int fd, void* buf, uint32_t count) {
    if (fd < 3 || fd >= MAX_OPEN_FILES || !g_files[fd].used) return -1;
    open_file_t* f = &g_files[fd];
    
    if (f->cursor >= f->size) return 0;
    
    uint32_t avail = f->size - f->cursor;
    uint32_t to_read = (count > avail) ? avail : count;
    
    // Copy memory
    uint8_t* dst = (uint8_t*)buf;
    uint8_t* src = ((uint8_t*)f->data) + f->cursor;
    for(uint32_t i=0; i<to_read; i++) dst[i] = src[i];
    
    f->cursor += to_read;
    return to_read;
}

static int sys_seek(int fd, int offset, int whence) {
    if (fd < 3 || fd >= MAX_OPEN_FILES || !g_files[fd].used) return -1;
    open_file_t* f = &g_files[fd];
    
    if (whence == 0) { // SEEK_SET
        f->cursor = offset;
    } else if (whence == 1) { // SEEK_CUR
        f->cursor += offset;
    } else if (whence == 2) { // SEEK_END
        f->cursor = f->size + offset;
    }
    if (f->cursor > f->size) f->cursor = f->size;
    return f->cursor;
}

static int sys_close(int fd) {
    if (fd < 3 || fd >= MAX_OPEN_FILES || !g_files[fd].used) return -1;
    open_file_t* f = &g_files[fd];
    kfree(f->data);
    f->used = 0;
    return 0;
}

static void* sys_mmap(void* addr, uint32_t length) {
    uint32_t pages = (length + 4095) / 4096;
    uint64_t vaddr = (uint64_t)addr;
    if (vaddr == 0) {
        // Allocate dynamically at some high address for user
        vaddr = 0x4000000000; 
    }
    for(uint32_t i=0; i<pages; i++) {
        void* paddr = PMM_AllocatePage();
        if (!paddr) return (void*)-1;
        VMM_MapPage((void*)(vaddr + (i*4096)), paddr, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
    }
    return (void*)vaddr;
}

uint64_t syscall_handler(uint64_t rsp) {
  struct InterruptFrame *f = (struct InterruptFrame *)rsp;

  // Syscall convention (v0):
  // rax = syscall number
  // rdi = arg0
  // rsi = arg1
  // rdx = arg2
  uint64_t num = f->rax;
  if (num == 0) {
    serial_print("[USERMODE] exiting ring3\n");
    return Task_Exit(rsp);
  } else if (num == 1) {
    sys_write_user((const char *)f->rdi, f->rsi);
    f->rax = 0;
  } else if (num == 2) {
    f->rax = (uint64_t)sys_mmap((void*)f->rdi, (uint32_t)f->rsi);
  } else if (num == 3) {
    f->rax = sys_open((const char*)f->rdi);
  } else if (num == 4) {
    f->rax = sys_read((int)f->rdi, (void*)f->rsi, (uint32_t)f->rdx);
  } else if (num == 5) {
    f->rax = sys_close((int)f->rdi);
  } else if (num == 6) {
    f->rax = sys_seek((int)f->rdi, (int)f->rsi, (int)f->rdx);
  } else if (num == 7) {
    f->rax = Timer_Ticks();
  } else if (num == 8) {
    char c = 0;
    if (input_kbd_pop_char(&c)) {
        f->rax = (uint64_t)c;
    } else {
        f->rax = 0;
    }
  } else if (num == 9) {
    extern void desktop_draw_frame(const uint32_t* src, uint32_t w, uint32_t h);
    desktop_draw_frame((const uint32_t*)f->rdi, (uint32_t)f->rsi, (uint32_t)f->rdx);
    f->rax = 0;
  } else {
    f->rax = (uint64_t)-1;
  }

  return rsp;
}
