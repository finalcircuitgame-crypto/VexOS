#include "../../include/interrupts.h"
#include "../../include/heap.h"
#include "../../include/simplefs.h"
#include "../../include/vmm.h"
#include "../../include/pmm.h"
#include "../../include/input.h"
#include <stdint.h>

void serial_print(const char *str);
extern uint64_t Task_Exit(uint64_t current_rsp);
extern uint64_t Timer_Ticks(void);
extern void desktop_draw_frame(const uint32_t *src, uint32_t w, uint32_t h);

#define MAX_OPEN_FILES 16
#define USER_FILENAME_MAX 32u
#define USER_WRITE_MAX 4096u
#define USER_MMAP_MAX (16u * 1024u * 1024u)
#define USER_MMAP_BASE 0x0000004000000000ULL
#define USER_ADDRESS_LIMIT 0x0000800000000000ULL

typedef struct {
    int used;
    void *data;
    uint32_t size;
    uint32_t cursor;
} open_file_t;

static open_file_t g_files[MAX_OPEN_FILES];
static uint64_t g_next_mmap = USER_MMAP_BASE;

static int copy_user_string(char *dst, uint32_t dst_cap, const char *src) {
    if (!dst || dst_cap == 0 || !src) return 0;

    uintptr_t base = (uintptr_t)src;
    for (uint32_t i = 0; i < dst_cap; i++) {
        if (base > UINTPTR_MAX - i) return 0;
        const char *at = (const char *)(base + i);
        if (!VMM_IsUserRange(at, 1, 0)) return 0;
        dst[i] = *at;
        if (dst[i] == '\0') return 1;
    }

    dst[0] = '\0';
    return 0;
}

static int sys_write_user(const char *s, uint64_t n) {
    if (n == 0) return 0;
    if (n > USER_WRITE_MAX) n = USER_WRITE_MAX;
    if (!VMM_IsUserRange(s, n, 0)) return -1;

    for (uint64_t i = 0; i < n; i++) {
        char c = s[i];
        char out[2] = {c, 0};
        serial_print(out);
    }
    return (int)n;
}

static int sys_open(const char *filename) {
    char name_buf[USER_FILENAME_MAX];
    if (!copy_user_string(name_buf, sizeof(name_buf), filename)) return -1;

    int fd = -1;
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (!g_files[i].used) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return -1;

    uint32_t size = 0;
    if (!simplefs_file_size(name_buf, &size)) return -1;

    // Keep zero-length files openable even if the allocator treats size 0 as
    // an invalid allocation request.
    void *data = kmalloc(size ? (size_t)size : 1u);
    if (!data) return -1;

    uint32_t out_len = 0;
    if (!simplefs_read_file(name_buf, data, size, &out_len) || out_len != size) {
        kfree(data);
        return -1;
    }

    g_files[fd].used = 1;
    g_files[fd].data = data;
    g_files[fd].size = size;
    g_files[fd].cursor = 0;
    return fd;
}

static int sys_read(int fd, void *buf, uint32_t count) {
    if (fd < 3 || fd >= MAX_OPEN_FILES || !g_files[fd].used) return -1;

    open_file_t *file = &g_files[fd];
    if (file->cursor >= file->size || count == 0) return 0;

    uint32_t available = file->size - file->cursor;
    uint32_t to_read = count > available ? available : count;
    if (!VMM_IsUserRange(buf, to_read, 1)) return -1;

    uint8_t *dst = (uint8_t *)buf;
    const uint8_t *src = (const uint8_t *)file->data + file->cursor;
    for (uint32_t i = 0; i < to_read; i++) dst[i] = src[i];

    file->cursor += to_read;
    return (int)to_read;
}

static int sys_seek(int fd, int offset, int whence) {
    if (fd < 3 || fd >= MAX_OPEN_FILES || !g_files[fd].used) return -1;

    open_file_t *file = &g_files[fd];
    int64_t base;
    if (whence == 0) base = 0;
    else if (whence == 1) base = file->cursor;
    else if (whence == 2) base = file->size;
    else return -1;

    int64_t target = base + (int64_t)offset;
    if (target < 0 || target > (int64_t)file->size) return -1;

    file->cursor = (uint32_t)target;
    return (int)file->cursor;
}

static int sys_close(int fd) {
    if (fd < 3 || fd >= MAX_OPEN_FILES || !g_files[fd].used) return -1;

    kfree(g_files[fd].data);
    g_files[fd].data = NULL;
    g_files[fd].size = 0;
    g_files[fd].cursor = 0;
    g_files[fd].used = 0;
    return 0;
}

static void *sys_mmap(void *requested_addr, uint32_t length) {
    if (length == 0 || length > USER_MMAP_MAX) return (void *)-1;

    uint64_t pages = ((uint64_t)length + PAGE_SIZE - 1u) / PAGE_SIZE;
    uint64_t bytes = pages * PAGE_SIZE;
    uint64_t vaddr = (uint64_t)(uintptr_t)requested_addr;

    if (vaddr == 0) {
        vaddr = (g_next_mmap + PAGE_SIZE - 1u) & ~(uint64_t)(PAGE_SIZE - 1u);
    } else if (vaddr & (PAGE_SIZE - 1u)) {
        return (void *)-1;
    }

    if (vaddr < USER_MMAP_BASE || vaddr >= USER_ADDRESS_LIMIT ||
        bytes > USER_ADDRESS_LIMIT - vaddr) return (void *)-1;

    uint64_t mapped = 0;
    for (; mapped < pages; mapped++) {
        void *physical = PMM_AllocatePage();
        if (!physical) break;

        // Never expose stale kernel/previous-process contents to ring 3.
        uint64_t *clear_words = (uint64_t *)physical;
        for (uint32_t word = 0; word < PAGE_SIZE / sizeof(uint64_t); word++)
            clear_words[word] = 0;

        void *virtual_page = (void *)(uintptr_t)(vaddr + mapped * PAGE_SIZE);
        if (!VMM_MapPage(virtual_page, physical,
                        PAGE_USER | PAGE_WRITE | PAGE_PRESENT)) {
            PMM_FreePage(physical);
            break;
        }
    }

    if (mapped != pages) {
        while (mapped > 0) {
            mapped--;
            VMM_UnmapPage((void *)(uintptr_t)(vaddr + mapped * PAGE_SIZE), 1);
        }
        return (void *)-1;
    }

    if (vaddr == g_next_mmap || vaddr >= g_next_mmap)
        g_next_mmap = vaddr + bytes;
    return (void *)(uintptr_t)vaddr;
}

uint64_t syscall_handler(uint64_t rsp) {
    struct InterruptFrame *frame = (struct InterruptFrame *)rsp;
    if (!frame) return rsp;

    // ABI: rax = syscall number; rdi/rsi/rdx = arguments.
    switch (frame->rax) {
    case 0:
        serial_print("[USERMODE] exiting ring3\n");
        return Task_Exit(rsp);
    case 1:
        frame->rax = (uint64_t)(int64_t)sys_write_user(
            (const char *)(uintptr_t)frame->rdi, frame->rsi);
        break;
    case 2:
        frame->rax = (uint64_t)(uintptr_t)sys_mmap(
            (void *)(uintptr_t)frame->rdi, (uint32_t)frame->rsi);
        break;
    case 3:
        frame->rax = (uint64_t)(int64_t)sys_open(
            (const char *)(uintptr_t)frame->rdi);
        break;
    case 4:
        frame->rax = (uint64_t)(int64_t)sys_read(
            (int)frame->rdi, (void *)(uintptr_t)frame->rsi,
            (uint32_t)frame->rdx);
        break;
    case 5:
        frame->rax = (uint64_t)(int64_t)sys_close((int)frame->rdi);
        break;
    case 6:
        frame->rax = (uint64_t)(int64_t)sys_seek(
            (int)frame->rdi, (int)frame->rsi, (int)frame->rdx);
        break;
    case 7:
        frame->rax = Timer_Ticks();
        break;
    case 8: {
        char c = 0;
        frame->rax = input_kbd_pop_char(&c) ? (uint64_t)(uint8_t)c : 0;
        break;
    }
    // Returns 0 when empty, otherwise bit 16 marks a valid event,
    // bit 8 is pressed/released, and bits 0..7 contain the HID usage.
    case 10: {
        input_kbd_event_t event;
        if (input_kbd_pop_event(&event))
            frame->rax = 0x10000ULL | ((uint64_t)event.pressed << 8) | event.usage;
        else
            frame->rax = 0;
        break;
    }
    case 9: {
        uint64_t w = frame->rsi;
        uint64_t h = frame->rdx;
        if (w == 0 || h == 0 || w > 8192 || h > 8192 ||
            w * h > (4ULL * 1024ULL * 1024ULL) ||
            !VMM_IsUserRange((const void *)(uintptr_t)frame->rdi,
                             w * h * sizeof(uint32_t), 0)) {
            frame->rax = (uint64_t)-1;
        } else {
            desktop_draw_frame((const uint32_t *)(uintptr_t)frame->rdi,
                               (uint32_t)w, (uint32_t)h);
            frame->rax = 0;
        }
        break;
    }
    default:
        frame->rax = (uint64_t)-1;
        break;
    }

    return rsp;
}
