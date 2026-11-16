#include "../../include/process.h"
#include "../../include/pmm.h"
#include "../../include/vmm.h"
#include <stddef.h>
#include <stdint.h>

#define PROCESS_MAX TASK_MAX

typedef struct {
  ProcessInfo info;
  AddressSpace *address_space;
  void *kernel_stack_base;
  uint8_t owns_kernel_stack;
} Process;

static Process processes[PROCESS_MAX];
static ProcessId next_pid = 1;
static uint8_t process_manager_ready;

static void copy_name(char dst[PROCESS_NAME_MAX], const char *src) {
  uint32_t i = 0;
  if (!src)
    src = "unnamed";
  for (; i + 1u < PROCESS_NAME_MAX && src[i]; ++i)
    dst[i] = src[i];
  dst[i] = '\0';
  for (++i; i < PROCESS_NAME_MAX; ++i)
    dst[i] = '\0';
}

static Process *find_by_pid(ProcessId pid) {
  if (!pid)
    return NULL;
  for (uint32_t i = 0; i < PROCESS_MAX; ++i) {
    if (processes[i].info.state != PROCESS_UNUSED &&
        processes[i].info.pid == pid)
      return &processes[i];
  }
  return NULL;
}

static Process *find_by_task(uint32_t task_id) {
  for (uint32_t i = 0; i < PROCESS_MAX; ++i) {
    if (processes[i].info.state != PROCESS_UNUSED &&
        processes[i].info.task_id == task_id)
      return &processes[i];
  }
  return NULL;
}

static Process *allocate_record(void) {
  for (uint32_t i = 0; i < PROCESS_MAX; ++i) {
    if (processes[i].info.state == PROCESS_UNUSED)
      return &processes[i];
  }
  return NULL;
}

static ProcessId current_parent_pid(void) {
  Process *current = find_by_task(Task_GetCurrentId());
  return current ? current->info.pid : 0;
}

void Process_Init(void) {
  for (uint32_t i = 0; i < PROCESS_MAX; ++i) {
    processes[i].info.pid = 0;
    processes[i].info.parent_pid = 0;
    processes[i].info.task_id = 0;
    processes[i].info.state = PROCESS_UNUSED;
    processes[i].info.exit_code = 0;
    processes[i].info.user_mode = 0;
    processes[i].info.name[0] = '\0';
    processes[i].address_space = NULL;
    processes[i].kernel_stack_base = NULL;
    processes[i].owns_kernel_stack = 0;
  }

  next_pid = 1;
  process_manager_ready = 1;

  Process *system = allocate_record();
  if (!system) {
    process_manager_ready = 0;
    return;
  }
  system->info.pid = next_pid++;
  system->info.parent_pid = 0;
  system->info.task_id = Task_GetCurrentId();
  system->info.state = PROCESS_RUNNING;
  system->info.exit_code = 0;
  system->info.user_mode = 0;
  copy_name(system->info.name, "system");
}

ProcessId Process_RegisterTask(const char *name, uint32_t task_id,
                               void *external_stack_base) {
  if (!process_manager_ready || task_id >= TASK_MAX ||
      task_id == Task_GetCurrentId() || find_by_task(task_id))
    return 0;

  Process *process = allocate_record();
  if (!process)
    return 0;

  process->info.pid = next_pid++;
  if (!next_pid)
    next_pid = 1;
  process->info.parent_pid = current_parent_pid();
  process->info.task_id = task_id;
  process->info.state = PROCESS_READY;
  process->info.exit_code = 0;
  process->info.user_mode = 0;
  copy_name(process->info.name, name);
  process->address_space = NULL;
  process->kernel_stack_base = external_stack_base;
  process->owns_kernel_stack = 0;
  return process->info.pid;
}

ProcessId Process_CreateKernel(const char *name, void (*entry)(void)) {
  if (!entry)
    return 0;

  void *stack = PMM_AllocatePage();
  if (!stack)
    return 0;
  uint64_t *words = (uint64_t *)stack;
  for (uint32_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); ++i)
    words[i] = 0;

  int32_t task_id = Task_Create(entry, (uint8_t *)stack + PAGE_SIZE);
  if (task_id < 0) {
    PMM_FreePage(stack);
    return 0;
  }

  ProcessId pid = Process_RegisterTask(name, (uint32_t)task_id, stack);
  if (!pid) {
    Task_Discard((uint32_t)task_id);
    PMM_FreePage(stack);
    return 0;
  }
  Process *process = find_by_pid(pid);
  process->owns_kernel_stack = 1;
  return pid;
}

ProcessId Process_CreateUser(const char *name, AddressSpace *space,
                             uint64_t entry, uint64_t user_stack_top) {
  if (!space || !entry || !user_stack_top)
    return 0;

  void *kernel_stack = PMM_AllocatePage();
  if (!kernel_stack)
    return 0;
  uint64_t *words = (uint64_t *)kernel_stack;
  for (uint32_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); ++i)
    words[i] = 0;

  if (!VMM_AddressSpaceMapPage(space, kernel_stack, kernel_stack,
                               PAGE_PRESENT | PAGE_WRITE)) {
    PMM_FreePage(kernel_stack);
    return 0;
  }

  int32_t task_id = Task_CreateUser(space, entry, user_stack_top,
                                    (uint8_t *)kernel_stack + PAGE_SIZE);
  if (task_id < 0) {
    PMM_FreePage(kernel_stack);
    return 0;
  }

  ProcessId pid = Process_RegisterTask(name, (uint32_t)task_id, kernel_stack);
  if (!pid) {
    Task_Discard((uint32_t)task_id);
    PMM_FreePage(kernel_stack);
    return 0;
  }

  Process *process = find_by_pid(pid);
  process->address_space = space;
  process->owns_kernel_stack = 1;
  process->info.user_mode = 1;
  return pid;
}

int Process_BeginUserModeCurrent(AddressSpace *space,
                                 uint64_t kernel_stack_top) {
  if (!process_manager_ready || !space || !kernel_stack_top)
    return 0;
  Process *process = find_by_task(Task_GetCurrentId());
  if (!process || process->info.state != PROCESS_RUNNING)
    return 0;

  process->address_space = space;
  process->info.user_mode = 1;
  if (!Task_SetCurrentContext(space, kernel_stack_top, 1)) {
    process->address_space = NULL;
    process->info.user_mode = 0;
    return 0;
  }
  return 1;
}

void Process_DetachCurrentAddressSpace(void) {
  Process *process = find_by_task(Task_GetCurrentId());
  if (!process)
    return;
  process->address_space = NULL;
  Task_SetCurrentContext(NULL, 0, process->info.user_mode);
  VMM_Activate();
}

int Process_IsCurrentUser(void) {
  Process *process = find_by_task(Task_GetCurrentId());
  return process && process->info.user_mode &&
         process->info.state == PROCESS_RUNNING;
}

ProcessId Process_GetCurrentId(void) {
  Process *process = find_by_task(Task_GetCurrentId());
  return process ? process->info.pid : 0;
}

uint32_t Process_GetCount(void) {
  uint32_t count = 0;
  for (uint32_t i = 0; i < PROCESS_MAX; ++i) {
    if (processes[i].info.state != PROCESS_UNUSED)
      ++count;
  }
  return count;
}

int Process_GetInfo(ProcessId pid, ProcessInfo *out) {
  if (!out)
    return 0;
  Process *process = find_by_pid(pid);
  if (!process)
    return 0;
  *out = process->info;
  return 1;
}

void Process_OnTaskSwitch(uint32_t previous_task, uint32_t next_task) {
  if (!process_manager_ready)
    return;
  Process *previous = find_by_task(previous_task);
  Process *next = find_by_task(next_task);
  if (previous && previous->info.state == PROCESS_RUNNING)
    previous->info.state = PROCESS_READY;
  if (next && next->info.state == PROCESS_READY)
    next->info.state = PROCESS_RUNNING;
}

void Process_OnTaskExit(uint32_t task_id) {
  Process *process = find_by_task(task_id);
  if (!process || process->info.state == PROCESS_ZOMBIE)
    return;
  process->info.state = PROCESS_ZOMBIE;
  process->info.exit_code = -1;
}

uint64_t Process_ExitCurrent(uint64_t saved_rsp, int32_t exit_code) {
  uint32_t task_id = Task_GetCurrentId();
  Process *process = find_by_task(task_id);
  if (!process || !process->info.user_mode)
    return Task_Exit(saved_rsp);

  if (process->address_space) {
    AddressSpace *space = process->address_space;
    process->address_space = NULL;
    Task_SetCurrentContext(NULL, 0, 1);
    VMM_Activate();
    VMM_AddressSpaceDestroy(space);
  }

  process->info.exit_code = exit_code;
  process->info.state = PROCESS_ZOMBIE;

  Process *parent = find_by_pid(process->info.parent_pid);
  if (parent && parent->info.state == PROCESS_BLOCKED) {
    parent->info.state = PROCESS_READY;
    Task_MakeReady(parent->info.task_id);
  }

  return Task_Exit(saved_rsp);
}

int Process_Wait(ProcessId pid, int32_t *exit_code) {
  if (!pid || pid == Process_GetCurrentId())
    return -1;

  for (;;) {
    Process *child = find_by_pid(pid);
    if (!child || child->info.parent_pid != Process_GetCurrentId())
      return -1;
    if (child->info.state == PROCESS_ZOMBIE) {
      if (exit_code)
        *exit_code = child->info.exit_code;
      return Process_ReapTask(child->info.task_id) ? 0 : -1;
    }

    Process *current = find_by_task(Task_GetCurrentId());
    if (!current || current->info.state != PROCESS_RUNNING)
      return -1;
    current->info.state = PROCESS_BLOCKED;
    Task_BlockCurrent();
    Task_Yield();
  }
}

int Process_ReapTask(uint32_t task_id) {
  Process *process = find_by_task(task_id);
  if (!process || process->info.state != PROCESS_ZOMBIE ||
      task_id == Task_GetCurrentId())
    return 0;

  if (process->address_space) {
    VMM_AddressSpaceDestroy(process->address_space);
    process->address_space = NULL;
  }
  if (process->owns_kernel_stack && process->kernel_stack_base)
    PMM_FreePage(process->kernel_stack_base);
  if (!Task_Reap(task_id))
    return 0;

  process->info.pid = 0;
  process->info.parent_pid = 0;
  process->info.task_id = 0;
  process->info.state = PROCESS_UNUSED;
  process->info.exit_code = 0;
  process->info.user_mode = 0;
  process->info.name[0] = '\0';
  process->kernel_stack_base = NULL;
  process->owns_kernel_stack = 0;
  return 1;
}
