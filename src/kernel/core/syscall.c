#include "../../include/interrupts.h"
#include "../../include/process.h"
#include "../../include/heap.h"
#include "../../include/vfs.h"
#include "../../include/vmm.h"
#include "../../include/pmm.h"
#include "../../include/desktop.h"
#include "../../include/input.h"
#include <stdint.h>

void serial_print(const char *str);
extern uint64_t Task_Exit(uint64_t current_rsp);
extern void usermode_cleanup_current(void);
extern void desktop_user_app_exited(int faulted);
extern uint64_t Timer_Ticks(void);
extern int desktop_draw_frame(const uint32_t *src, uint32_t w, uint32_t h);
extern void xhci_poll_events(void);

#define MAX_OPEN_FILES 16
#define USER_FILENAME_MAX VFS_PATH_MAX
#define USER_WRITE_MAX 4096u
#define USER_FILE_WRITE_MAX (1024u * 1024u)
#define USER_MMAP_MAX (16u * 1024u * 1024u)
#define USER_MMAP_BASE 0x0000004000000000ULL
#define USER_MMAP_TRACKED_PAGES 4096u
#define USER_ADDRESS_LIMIT 0x0000800000000000ULL

typedef struct {
    int used;
    uint8_t is_directory;
    vfs_file_t file;
    vfs_dir_t directory;
} open_file_t;

typedef struct {
    open_file_t files[MAX_OPEN_FILES];
    uint64_t next_mmap;
    uint32_t mmap_page_count;
    uint8_t initialized;
} syscall_process_state_t;

static syscall_process_state_t g_process_states[TASK_MAX];

static syscall_process_state_t *syscall_current_state(void) {
    uint32_t task_id = Task_GetCurrentId();
    if (task_id >= TASK_MAX)
        return NULL;

    syscall_process_state_t *state = &g_process_states[task_id];
    if (!state->initialized) {
        state->next_mmap = USER_MMAP_BASE;
        state->mmap_page_count = 0;
        state->initialized = 1;
    }
    return state;
}

static int16_t clamp_mouse_axis(int32_t v) {
    if(v>32767)return 32767;if(v<-32768)return -32768;return (int16_t)v;
}

/* Release only the descriptors and mmap accounting belonging to this task.
   User frames themselves are reclaimed by its AddressSpace teardown. */
void syscall_process_cleanup_task(uint32_t task_id) {
    if (task_id >= TASK_MAX)
        return;

    syscall_process_state_t *state = &g_process_states[task_id];
    for (int fd = 0; fd < MAX_OPEN_FILES; ++fd) {
        if (state->files[fd].used) {
            if (state->files[fd].is_directory)
                vfs_dir_close(&state->files[fd].directory);
            else
                vfs_close(&state->files[fd].file);
        }
        state->files[fd].used = 0;
        state->files[fd].is_directory = 0;
    }

    state->mmap_page_count = 0;
    state->next_mmap = USER_MMAP_BASE;
    state->initialized = 0;
}

void syscall_process_cleanup(void) {
    syscall_process_cleanup_task(Task_GetCurrentId());
}

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
    syscall_process_state_t *state = syscall_current_state();
    if (!state) return -1;
    char name_buf[USER_FILENAME_MAX];
    if (!copy_user_string(name_buf, sizeof(name_buf), filename)) return -1;

    int fd = -1;
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (!state->files[i].used) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return -1;

    state->files[fd].file = (vfs_file_t){0};
    if (!vfs_open(&state->files[fd].file, name_buf)) {
        serial_print("[USERMODE] VFS open failed: ");
        serial_print(name_buf);
        serial_print("\\n");
        return -1;
    }
    state->files[fd].is_directory = 0;
    state->files[fd].used = 1;
    return fd;
}

static int sys_read(int fd, void *buf, uint32_t count) {
    syscall_process_state_t *state = syscall_current_state();
    if (!state || fd < 3 || fd >= MAX_OPEN_FILES ||
        !state->files[fd].used || state->files[fd].is_directory)
        return -1;

    vfs_file_t *file = &state->files[fd].file;
    if (!file->is_open)
        return -1;
    if (count == 0)
        return 0;

    uint32_t available = vfs_file_remaining(file);
    if (available == 0)
        return 0;
    uint32_t wanted = count < available ? count : available;
    if (!VMM_IsUserRange(buf, wanted, 1))
        return -1;

    return vfs_file_read(file, buf, wanted);
}

static int sys_seek(int fd, int offset, int whence) {
    syscall_process_state_t *state = syscall_current_state();
    if (!state || fd < 3 || fd >= MAX_OPEN_FILES ||
        !state->files[fd].used || state->files[fd].is_directory)
        return -1;
    int64_t result = vfs_file_seek(&state->files[fd].file,
                                   (int64_t)offset, whence);
    if (result < 0 || result > 0x7FFFFFFF)
        return -1;
    return (int)result;
}

static int sys_close(int fd) {
    syscall_process_state_t *state = syscall_current_state();
    if (!state || fd < 3 || fd >= MAX_OPEN_FILES ||
        !state->files[fd].used)
        return -1;
    int result = state->files[fd].is_directory
        ? vfs_dir_close(&state->files[fd].directory)
        : vfs_close(&state->files[fd].file);
    if (!result)
        return -1;
    state->files[fd].used = 0;
    state->files[fd].is_directory = 0;
    return 0;
}

static int sys_find_free_fd(syscall_process_state_t *state) {
    if (!state)
        return -1;
    for (int fd = 3; fd < MAX_OPEN_FILES; ++fd) {
        if (!state->files[fd].used)
            return fd;
    }
    return -1;
}

static int sys_vfs_create(const char *user_path) {
    char path[USER_FILENAME_MAX];
    if (!copy_user_string(path, sizeof(path), user_path))
        return -1;
    return vfs_create(path) ? 0 : -1;
}

static int sys_vfs_unlink(const char *user_path) {
    char path[USER_FILENAME_MAX];
    if (!copy_user_string(path, sizeof(path), user_path))
        return -1;
    return vfs_unlink(path) ? 0 : -1;
}

static int sys_vfs_write(const char *user_path, const void *user_data,
                         uint32_t length) {
    char path[USER_FILENAME_MAX];
    if (!copy_user_string(path, sizeof(path), user_path) ||
        length > USER_FILE_WRITE_MAX)
        return -1;
    if (length == 0)
        return vfs_write(path, NULL, 0) ? 0 : -1;
    if (!VMM_IsUserRange(user_data, length, 0))
        return -1;

    void *kernel_copy = kmalloc(length);
    if (!kernel_copy)
        return -1;
    const uint8_t *source = (const uint8_t *)user_data;
    uint8_t *destination = (uint8_t *)kernel_copy;
    for (uint32_t i = 0; i < length; ++i)
        destination[i] = source[i];

    int result = vfs_write(path, kernel_copy, length);
    kfree(kernel_copy);
    return result ? 0 : -1;
}

static int sys_vfs_stat(const char *user_path, uint64_t *out_size) {
    char path[USER_FILENAME_MAX];
    vfs_stat_t info;
    if (!copy_user_string(path, sizeof(path), user_path) ||
        !vfs_stat(path, &info))
        return -1;
    if (out_size)
        *out_size = info.size;
    return (int)info.type;
}

static int sys_vfs_opendir(const char *user_path) {
    syscall_process_state_t *state = syscall_current_state();
    char path[USER_FILENAME_MAX];
    if (!state || !copy_user_string(path, sizeof(path), user_path))
        return -1;
    int fd = sys_find_free_fd(state);
    if (fd < 0)
        return -1;

    state->files[fd].directory = (vfs_dir_t){0};
    if (!vfs_dir_open(&state->files[fd].directory, path))
        return -1;
    state->files[fd].is_directory = 1;
    state->files[fd].used = 1;
    return fd;
}

static int sys_vfs_readdir(int fd, char *user_name, uint32_t capacity,
                           uint64_t *out_size) {
    syscall_process_state_t *state = syscall_current_state();
    if (!state || fd < 3 || fd >= MAX_OPEN_FILES ||
        !state->files[fd].used || !state->files[fd].is_directory ||
        !user_name || capacity == 0 || capacity > VFS_NAME_MAX ||
        !VMM_IsUserRange(user_name, capacity, 1))
        return -1;

    char name[VFS_NAME_MAX];
    vfs_stat_t info;
    int result = vfs_dir_read(&state->files[fd].directory, name,
                              sizeof(name), &info);
    if (result <= 0)
        return result;
    if (info.type != VFS_NODE_FILE && info.type != VFS_NODE_DIRECTORY)
        return -1;

    uint32_t length = 0;
    while (length < VFS_NAME_MAX && name[length])
        ++length;
    if (length >= capacity)
        return -1;
    for (uint32_t i = 0; i <= length; ++i)
        user_name[i] = name[i];
    if (out_size)
        *out_size = info.size;
    return (int)info.type;
}

static void *sys_mmap(void *requested_addr, uint32_t length) {
    syscall_process_state_t *state = syscall_current_state();
    if (!state || length == 0 || length > USER_MMAP_MAX) return (void *)-1;

    uint64_t pages = ((uint64_t)length + PAGE_SIZE - 1u) / PAGE_SIZE;
    uint64_t bytes = pages * PAGE_SIZE;
    if (pages > USER_MMAP_TRACKED_PAGES - state->mmap_page_count)
        return (void *)-1;
    uint64_t vaddr = (uint64_t)(uintptr_t)requested_addr;

    if (vaddr == 0) {
        vaddr = (state->next_mmap + PAGE_SIZE - 1u) & ~(uint64_t)(PAGE_SIZE - 1u);
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

    state->mmap_page_count += (uint32_t)pages;

    if (vaddr >= state->next_mmap)
        state->next_mmap = vaddr + bytes;
    return (void *)(uintptr_t)vaddr;
}

uint64_t syscall_handler(uint64_t rsp) {
    struct InterruptFrame *frame = (struct InterruptFrame *)rsp;
    if (!frame) return rsp;

    // ABI: rax = syscall number; rdi/rsi/rdx = arguments.
    switch (frame->rax) {
    case 0:
        usermode_cleanup_current();
        syscall_process_cleanup();
        desktop_user_app_exited(0);
        serial_print("[USERMODE] exiting ring3\n");
        if (Process_IsCurrentUser())
            return Process_ExitCurrent(rsp, 0);
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
    // Keyboard events use bit 16 as valid, bit 8 as pressed/released,
    // and bits 0..7 as the USB HID usage.
    case 11:
        frame->rax = desktop_doom_app_ready() ? 0u : (uint64_t)-1;
        break;
    case 10: {
        /* int 0x80 is an interrupt gate: this poll cannot be preempted by PIT. */
        xhci_poll_events();
        input_kbd_event_t event;
        if(!desktop_doom_input_active()) {
            frame->rax=0;
            while(input_kbd_pop_event(&event)) {
                if(!event.pressed) {
                    frame->rax=0x10000ULL|event.usage;
                    break;
                }
            }
        } else if(input_kbd_pop_event(&event)) {
            frame->rax=0x10000ULL|((uint64_t)event.pressed<<8)|event.usage;
        } else frame->rax=0;
        break;
    }
    case 12: {
        /* int 0x80 is an interrupt gate: this poll cannot be preempted by PIT. */
        xhci_poll_events();
        input_pointer_state_t pointer=input_pointer_get();
        int32_t dx=0,dy=0;
        input_pointer_take_motion(&dx,&dy);
        uint32_t buttons=0;
        if(desktop_doom_mouse_input_active()) {
            if(desktop_doom_mouse_buttons_active()) buttons=pointer.buttons&7u;
        } else {dx=0;dy=0;}
        frame->rax=(uint64_t)(uint16_t)clamp_mouse_axis(dx)|
                   ((uint64_t)(uint16_t)clamp_mouse_axis(dy)<<16)|
                   ((uint64_t)buttons<<32);
        break;
    }
    case 13:
        frame->rax=desktop_doom_close_requested()?1u:0u;
        break;
    case 14:
        /* Refresh HID reports before returning the hardware's current
           arrow-key state. Doom uses this to recover a dropped key-up. */
        xhci_poll_events();
        frame->rax = input_kbd_arrow_state();
        break;
    case 15:
        /* Exit an independently managed user process without changing the desktop app. */
        usermode_cleanup_current();
        syscall_process_cleanup();
        if (Process_IsCurrentUser())
            return Process_ExitCurrent(rsp, (int32_t)frame->rdi);
        return Task_Exit(rsp);
    case 16:
        frame->rax = (uint64_t)(int64_t)sys_vfs_create(
            (const char *)(uintptr_t)frame->rdi);
        break;
    case 17:
        frame->rax = (uint64_t)(int64_t)sys_vfs_unlink(
            (const char *)(uintptr_t)frame->rdi);
        break;
    case 18:
        frame->rax = (uint64_t)(int64_t)sys_vfs_write(
            (const char *)(uintptr_t)frame->rdi,
            (const void *)(uintptr_t)frame->rsi,
            (uint32_t)frame->rdx);
        break;
    case 19: {
        uint64_t size = 0;
        int type = sys_vfs_stat((const char *)(uintptr_t)frame->rdi, &size);
        frame->rax = (uint64_t)(int64_t)type;
        frame->rdx = size;
        break;
    }
    case 20:
        frame->rax = (uint64_t)(int64_t)sys_vfs_opendir(
            (const char *)(uintptr_t)frame->rdi);
        break;
    case 21: {
        uint64_t size = 0;
        int type = sys_vfs_readdir(
            (int)frame->rdi, (char *)(uintptr_t)frame->rsi,
            (uint32_t)frame->rdx, &size);
        frame->rax = (uint64_t)(int64_t)type;
        frame->rdx = size;
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
            /* Syscall 9 submits a frame to the active Doom window surface;
               it is not a raw write to the hardware framebuffer. */
            frame->rax = desktop_draw_frame(
                (const uint32_t *)(uintptr_t)frame->rdi,
                (uint32_t)w, (uint32_t)h) ? 0u : (uint64_t)-1;
        }
        break;
    }
    default:
        frame->rax = (uint64_t)-1;
        break;
    }

    return rsp;
}
