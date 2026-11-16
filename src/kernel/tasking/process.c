#include "../../include/process.h"
#include "../../include/pmm.h"
#include "../../include/vmm.h"
#include "../../include/gdt.h"
#include <stddef.h>
#include <stdint.h>

#define PROCESS_MAX TASK_MAX

/* The scheduler runs on the PIT IRQ on this uniprocessor build. Keep a new
   task from running until its process record is completely published. */
static uint64_t process_irq_save_disable(void) {
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
  return flags;
}

static void process_irq_restore(uint64_t flags) {
  if (flags & (1ull << 9))
    __asm__ volatile("sti" ::: "memory");
}

typedef struct {
  ProcessInfo info;
  AddressSpace *address_space;
  void *kernel_stack_base;
  uint64_t kernel_stack_top;
  uint8_t owns_kernel_stack;
  uint8_t auto_reap;
} Process;

static Process processes[PROCESS_MAX];
static ProcessId next_pid = 1;
static ProcessId system_pid;
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

static ProcessId allocate_pid(void) {
  if (!next_pid)
    return 0;

  ProcessId pid = next_pid;
  /* Never wrap into a PID that may still belong to a live process. */
  next_pid = (next_pid == UINT64_MAX) ? 0 : next_pid + 1u;
  return pid;
}

static void process_wake_parent(Process *process) {
  if (!process)
    return;
  Process *parent = find_by_pid(process->info.parent_pid);
  if (parent && parent->info.state == PROCESS_BLOCKED) {
    parent->info.state = PROCESS_READY;
    Task_MakeReady(parent->info.task_id);
  }
}

static void process_reparent_children(Process *parent) {
  if (!parent || !system_pid || parent->info.pid == system_pid)
    return;

  for (uint32_t i = 0; i < PROCESS_MAX; ++i) {
    Process *child = &processes[i];
    if (child->info.state == PROCESS_UNUSED ||
        child->info.parent_pid != parent->info.pid)
      continue;

    /* The permanent system process adopts children of exiting parents.
       Adopted zombies are reclaimed by Process_ReapOrphans in system context. */
    child->info.parent_pid = system_pid;
    child->auto_reap = 1;
  }
}

static void process_mark_exited(Process *process, int32_t exit_code) {
  if (!process || process->info.state == PROCESS_ZOMBIE)
    return;

  process->info.state = PROCESS_ZOMBIE;
  process->info.exit_code = exit_code;
  process_reparent_children(process);
  process_wake_parent(process);
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
    processes[i].kernel_stack_top = 0;
    processes[i].owns_kernel_stack = 0;
    processes[i].auto_reap = 0;
  }

  next_pid = 1;
  system_pid = 0;
  process_manager_ready = 1;

  Process *system = allocate_record();
  if (!system) {
    process_manager_ready = 0;
    return;
  }
  system->info.pid = allocate_pid();
  if (!system->info.pid) {
    process_manager_ready = 0;
    return;
  }
  system_pid = system->info.pid;
  system->info.parent_pid = 0;
  system->info.task_id = Task_GetCurrentId();
  system->info.state = PROCESS_RUNNING;
  system->info.exit_code = 0;
  system->info.user_mode = 0;
  copy_name(system->info.name, "system");
}

ProcessId Process_RegisterTask(const char *name, uint32_t task_id,
                               void *external_stack_base) {
  uint64_t irq_flags = process_irq_save_disable();
  if (!process_manager_ready || task_id >= TASK_MAX ||
      task_id == Task_GetCurrentId() || find_by_task(task_id) ||
      !external_stack_base || !Task_GetTaskKernelStackTop(task_id)) {
    process_irq_restore(irq_flags);
    return 0;
  }

  Process *process = allocate_record();
  if (!process) {
    process_irq_restore(irq_flags);
    return 0;
  }

  ProcessId pid = allocate_pid();
  if (!pid) {
    process_irq_restore(irq_flags);
    return 0;
  }

  ProcessId parent_pid = current_parent_pid();
  if (!parent_pid)
    parent_pid = system_pid;

  process->info.pid = pid;
  process->info.parent_pid = parent_pid;
  process->info.task_id = task_id;
  process->info.state = PROCESS_READY;
  process->info.exit_code = 0;
  process->info.user_mode = 0;
  copy_name(process->info.name, name);
  process->address_space = NULL;
  process->kernel_stack_base = external_stack_base;
  process->kernel_stack_top = Task_GetTaskKernelStackTop(task_id);
  process->owns_kernel_stack = 0;
  process->auto_reap = 0;
  process_irq_restore(irq_flags);
  return pid;
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

  uint64_t irq_flags = process_irq_save_disable();
  int32_t task_id = Task_Create(entry, (uint8_t *)stack + PAGE_SIZE);
  if (task_id < 0) {
    process_irq_restore(irq_flags);
    PMM_FreePage(stack);
    return 0;
  }

  ProcessId pid = Process_RegisterTask(name, (uint32_t)task_id, stack);
  if (!pid) {
    Task_Discard((uint32_t)task_id);
    process_irq_restore(irq_flags);
    PMM_FreePage(stack);
    return 0;
  }
  Process *process = find_by_pid(pid);
  process->kernel_stack_top = (uint64_t)(uintptr_t)stack + PAGE_SIZE;
  process->owns_kernel_stack = 1;
  process_irq_restore(irq_flags);
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

  uint64_t irq_flags = process_irq_save_disable();
  int32_t task_id = Task_CreateUser(space, entry, user_stack_top,
                                    (uint8_t *)kernel_stack + PAGE_SIZE);
  if (task_id < 0) {
    process_irq_restore(irq_flags);
    VMM_AddressSpaceUnmapPage(space, kernel_stack, 0);
    PMM_FreePage(kernel_stack);
    return 0;
  }

  ProcessId pid = Process_RegisterTask(name, (uint32_t)task_id, kernel_stack);
  if (!pid) {
    Task_Discard((uint32_t)task_id);
    process_irq_restore(irq_flags);
    VMM_AddressSpaceUnmapPage(space, kernel_stack, 0);
    PMM_FreePage(kernel_stack);
    return 0;
  }

  Process *process = find_by_pid(pid);
  process->address_space = space;
  process->kernel_stack_top = (uint64_t)(uintptr_t)kernel_stack + PAGE_SIZE;
  process->owns_kernel_stack = 1;
  process->info.user_mode = 1;
  process_irq_restore(irq_flags);
  return pid;
}

int Process_AttachCurrentAddressSpace(AddressSpace *space) {
  Process *process = find_by_task(Task_GetCurrentId());
  if (!process || process->info.state != PROCESS_RUNNING ||
      !process->kernel_stack_top)
    return 0;

  /* This may attach a loading image before ring 3 is entered. */
  process->address_space = space;
  if (!Task_SetCurrentContext(space, process->kernel_stack_top,
                              process->info.user_mode)) {
    process->address_space = NULL;
    return 0;
  }
  return 1;
}

void Process_SetCurrentKernelStack(uint64_t stack_top) {
  Process *process = find_by_task(Task_GetCurrentId());
  if (!process)
    return;
  process->kernel_stack_top = stack_top;
  Task_SetCurrentContext(process->address_space, stack_top,
                         process->info.user_mode);
}

int Process_BeginUserModeCurrent(AddressSpace *space,
                                 uint64_t kernel_stack_top) {
  if (!process_manager_ready || !space || !kernel_stack_top)
    return 0;
  Process *process = find_by_task(Task_GetCurrentId());
  if (!process || process->info.state != PROCESS_RUNNING)
    return 0;

  process->address_space = space;
  process->kernel_stack_top = kernel_stack_top;
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
  Task_SetCurrentContext(NULL, process->kernel_stack_top, process->info.user_mode);
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
  uint64_t irq_flags = process_irq_save_disable();
  uint32_t count = 0;
  for (uint32_t i = 0; i < PROCESS_MAX; ++i) {
    if (processes[i].info.state != PROCESS_UNUSED)
      ++count;
  }
  process_irq_restore(irq_flags);
  return count;
}

int Process_GetInfo(ProcessId pid, ProcessInfo *out) {
  if (!out)
    return 0;
  uint64_t irq_flags = process_irq_save_disable();
  Process *process = find_by_pid(pid);
  if (!process) {
    process_irq_restore(irq_flags);
    return 0;
  }
  *out = process->info;
  process_irq_restore(irq_flags);
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
  process_mark_exited(process, -1);
}

__attribute__((noreturn)) void Process_ExitKernelCurrent(int32_t exit_code) {
  __asm__ volatile("cli" ::: "memory");
  Process *process = find_by_task(Task_GetCurrentId());
  process_mark_exited(process, exit_code);
  Task_ExitCurrent();
}

uint64_t Process_ExitCurrent(uint64_t saved_rsp, int32_t exit_code) {
  __asm__ volatile("cli" ::: "memory");
  uint32_t task_id = Task_GetCurrentId();
  Process *process = find_by_task(task_id);
  if (!process)
    return Task_Exit(saved_rsp);

  if (process->address_space) {
    AddressSpace *space = process->address_space;
    process->address_space = NULL;
    Task_SetCurrentContext(NULL, process->kernel_stack_top,
                           process->info.user_mode);
    VMM_Activate();
    VMM_AddressSpaceDestroy(space);
  }

  process_mark_exited(process, exit_code);
  return Task_Exit(saved_rsp);
}

int Process_Wait(ProcessId pid, int32_t *exit_code) {
  if (!pid)
    return -1;

  for (;;) {
    /* The child-state check and parent block must be one atomic operation.
       Otherwise the child can exit after the check but before cli(), leaving
       the parent blocked forever with nobody left to wake it. */
    uint64_t irq_flags = process_irq_save_disable();
    ProcessId self_pid = Process_GetCurrentId();
    if (pid == self_pid) {
      process_irq_restore(irq_flags);
      return -1;
    }

    Process *child = find_by_pid(pid);
    if (!child || child->info.parent_pid != self_pid) {
      process_irq_restore(irq_flags);
      return -1;
    }

    if (child->info.state == PROCESS_ZOMBIE) {
      uint32_t child_task_id = child->info.task_id;
      int32_t child_exit_code = child->info.exit_code;
      /* Keep the child record stable until its task slot and owned resources
         are released. Process_ReapTask preserves this disabled-IRQ state. */
      int reaped = Process_ReapTask(child_task_id);
      process_irq_restore(irq_flags);
      if (!reaped)
        return -1;
      if (exit_code)
        *exit_code = child_exit_code;
      return 0;
    }

    Process *current = find_by_task(Task_GetCurrentId());
    if (!current || current->info.state != PROCESS_RUNNING) {
      process_irq_restore(irq_flags);
      return -1;
    }

    current->info.state = PROCESS_BLOCKED;
    Task_BlockCurrent();
    /* Keep interrupts disabled until another task is running. The saved
       parent frame restores the original IF state when this task resumes. */
    Task_Yield();
    process_irq_restore(irq_flags);
  }
}

int Process_ReapTask(uint32_t task_id) {
  /* Validate under the same IRQ lock as task-slot removal. Only a process's
     parent (or the system process for adopted orphans) may reap the child. */
  uint64_t irq_flags = process_irq_save_disable();
  Process *process = find_by_task(task_id);
  ProcessId caller_pid = current_parent_pid();
  if (!process || process->info.state != PROCESS_ZOMBIE ||
      task_id == Task_GetCurrentId() || !caller_pid ||
      process->info.parent_pid != caller_pid ||
      (process->address_space &&
       VMM_GetActiveAddressSpace() == process->address_space)) {
    process_irq_restore(irq_flags);
    return 0;
  }

  /* The task is already off-CPU. Clear its scheduler slot before releasing
     the backing resources it referenced. */
  if (!Task_Reap(task_id)) {
    process_irq_restore(irq_flags);
    return 0;
  }
  if (process->address_space) {
    VMM_AddressSpaceDestroy(process->address_space);
    process->address_space = NULL;
  }
  if (process->owns_kernel_stack && process->kernel_stack_base)
    PMM_FreePage(process->kernel_stack_base);

  process->info.pid = 0;
  process->info.parent_pid = 0;
  process->info.task_id = 0;
  process->info.state = PROCESS_UNUSED;
  process->info.exit_code = 0;
  process->info.user_mode = 0;
  process->info.name[0] = '\0';
  process->kernel_stack_base = NULL;
  process->kernel_stack_top = 0;
  process->owns_kernel_stack = 0;
  process->auto_reap = 0;
  process_irq_restore(irq_flags);
  return 1;
}

void Process_ReapOrphans(void) {
  if (!system_pid || Process_GetCurrentId() != system_pid)
    return;

  for (uint32_t i = 0; i < PROCESS_MAX; ++i) {
    uint32_t task_id = UINT32_MAX;
    uint64_t irq_flags = process_irq_save_disable();
    Process *process = &processes[i];
    if (process->auto_reap &&
        process->info.parent_pid == system_pid &&
        process->info.state == PROCESS_ZOMBIE &&
        process->info.task_id != Task_GetCurrentId())
      task_id = process->info.task_id;
    process_irq_restore(irq_flags);

    if (task_id != UINT32_MAX)
      (void)Process_ReapTask(task_id);
  }
}

#ifdef VEXOS_CI_INPUT_TEST
static void ci_kernel_exit_entry(void) {
  Process_ExitKernelCurrent(42);
}

static volatile ProcessId ci_orphan_child_pid;

static void ci_orphan_leaf_entry(void) {
  /* Returning through the trampoline should yield exit code zero. */
}

static void ci_orphan_parent_entry(void) {
  ci_orphan_child_pid =
      Process_CreateKernel("ci-orphan-leaf", ci_orphan_leaf_entry);
}

static void ci_discard_unstarted(Process *process) {
  if (!process || process->info.state != PROCESS_READY)
    return;

  uint32_t task_id = process->info.task_id;
  if (!Task_Discard(task_id))
    return;

  AddressSpace *space = process->address_space;
  if (space && process->kernel_stack_base)
    (void)VMM_AddressSpaceUnmapPage(space, process->kernel_stack_base, 0);

  if (process->owns_kernel_stack && process->kernel_stack_base)
    PMM_FreePage(process->kernel_stack_base);

  process->address_space = NULL;
  process->kernel_stack_base = NULL;
  process->kernel_stack_top = 0;
  process->owns_kernel_stack = 0;
  process->auto_reap = 0;
  process->info.pid = 0;
  process->info.parent_pid = 0;
  process->info.task_id = 0;
  process->info.state = PROCESS_UNUSED;
  process->info.exit_code = 0;
  process->info.user_mode = 0;
  process->info.name[0] = '\0';

  if (space)
    VMM_AddressSpaceDestroy(space);
}

int Process_CI_SelfTest(void) {
  const uint64_t code_va = 0x0000600000000000ull;
  const uint64_t stack_va = code_va + PAGE_SIZE;
  const ProcessId parent_pid = Process_GetCurrentId();
  const uint32_t initial_count = Process_GetCount();

  if (!parent_pid || !process_manager_ready)
    return 0;

  int32_t ignored_exit_code = 0;
  if (Process_Wait(0, &ignored_exit_code) != -1 ||
      Process_Wait(parent_pid, &ignored_exit_code) != -1 ||
      Process_Wait(0xFFFFFFFFFFFFFFFEULL, &ignored_exit_code) != -1)
    return 0;

  AddressSpace *space = VMM_AddressSpaceCreate();
  if (!space)
    return 0;

  void *code_page = PMM_AllocatePage();
  void *user_stack_page = PMM_AllocatePage();
  if (!code_page || !user_stack_page) {
    if (code_page) PMM_FreePage(code_page);
    if (user_stack_page) PMM_FreePage(user_stack_page);
    VMM_AddressSpaceDestroy(space);
    return 0;
  }

  if (!VMM_AddressSpaceMapPage(space, (void *)(uintptr_t)code_va,
                              code_page, PAGE_USER | PAGE_PRESENT) ||
      !VMM_AddressSpaceMapPage(space, (void *)(uintptr_t)stack_va,
                              user_stack_page,
                              PAGE_USER | PAGE_WRITE | PAGE_PRESENT)) {
    /* Destroy only frees successfully installed user mappings. */
    if (!VMM_AddressSpaceIsUserRange(space,
            (const void *)(uintptr_t)code_va, 1, 0))
      PMM_FreePage(code_page);
    if (!VMM_AddressSpaceIsUserRange(space,
            (const void *)(uintptr_t)stack_va, 1, 1))
      PMM_FreePage(user_stack_page);
    VMM_AddressSpaceDestroy(space);
    return 0;
  }

  ProcessId user_pid = Process_CreateUser(
      "ci-user", space, code_va, stack_va + PAGE_SIZE);
  Process *user = find_by_pid(user_pid);
  ProcessInfo info;
  int user_ok = user_pid && user &&
      Process_GetInfo(user_pid, &info) &&
      info.parent_pid == parent_pid &&
      info.state == PROCESS_READY &&
      info.user_mode == 1u &&
      user->address_space == space &&
      user->kernel_stack_top != 0 &&
      VMM_AddressSpaceIsUserRange(space,
          (const void *)(uintptr_t)code_va, 1, 0) &&
      VMM_AddressSpaceIsUserRange(space,
          (const void *)(uintptr_t)(stack_va + PAGE_SIZE - 1u), 1, 1);

  /* Exercise the real scheduler path, not just the VMM activation helper.
     The synthetic saved RSP values are never dereferenced in this CI-only
     direct scheduler test; both task frames were validated above. */
  if (user_ok) {
    const uint32_t user_task_id = info.task_id;
    (void)Task_Schedule(0x11110000u);
    user_ok = Task_GetCurrentId() == user_task_id &&
        Process_GetCurrentId() == user_pid &&
        Process_IsCurrentUser() &&
        VMM_GetActiveAddressSpace() == space &&
        TSS_GetRSP0() == user->kernel_stack_top;

    (void)Task_Schedule(0x22220000u);
    user_ok = user_ok &&
        Task_GetCurrentId() == 0u &&
        Process_GetCurrentId() == parent_pid &&
        VMM_GetActiveAddressSpace() == NULL;
  }

  if (user)
    ci_discard_unstarted(user);
  else
    VMM_AddressSpaceDestroy(space);

  if (!user_ok || Process_GetCount() != initial_count)
    return 0;

  ProcessId kernel_pid = Process_CreateKernel("ci-kernel", ci_kernel_exit_entry);
  Process *kernel = find_by_pid(kernel_pid);
  int kernel_ok = kernel_pid && kernel &&
      Process_GetInfo(kernel_pid, &info) &&
      info.parent_pid == parent_pid &&
      info.state == PROCESS_READY &&
      info.user_mode == 0u &&
      kernel->kernel_stack_base != NULL &&
      kernel->kernel_stack_top != 0;

  if (kernel_ok) {
    /* Wait blocks the parent, runs the child, observes exit code 42, and
       reaps the zombie. A second wait must reject the now-invalid PID. */
    int32_t wait_exit_code = -999;
    kernel_ok = Process_Wait(kernel_pid, &wait_exit_code) == 0 &&
        wait_exit_code == 42 &&
        !find_by_pid(kernel_pid) &&
        Process_Wait(kernel_pid, NULL) == -1;
  } else if (kernel) {
    ci_discard_unstarted(kernel);
  }

  if (!kernel_ok || Process_GetCount() != initial_count ||
      Process_GetCurrentId() != parent_pid)
    return 0;

  /* Exit a parent while its child is still ready. The child must be adopted
     by the system process and automatically reaped after it exits. */
  ci_orphan_child_pid = 0;
  ProcessId orphan_parent_pid =
      Process_CreateKernel("ci-orphan-parent", ci_orphan_parent_entry);
  int32_t orphan_parent_exit = -999;
  if (!orphan_parent_pid ||
      Process_Wait(orphan_parent_pid, &orphan_parent_exit) != 0 ||
      orphan_parent_exit != 0 || !ci_orphan_child_pid)
    return 0;

  Process *orphan = find_by_pid(ci_orphan_child_pid);
  int orphan_ok = orphan &&
      orphan->info.parent_pid == parent_pid &&
      orphan->info.state == PROCESS_ZOMBIE &&
      orphan->auto_reap;
  Process_ReapOrphans();

  return orphan_ok && !find_by_pid(ci_orphan_child_pid) &&
         Process_GetCount() == initial_count &&
         Process_GetCurrentId() == parent_pid;
}
#endif
