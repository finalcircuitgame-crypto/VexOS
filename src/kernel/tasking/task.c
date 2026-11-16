#include "../../include/task.h"
#include "../../include/heap.h"
#include "../../include/vmm.h"
#include "../../include/gdt.h"
#include <stddef.h>
#include <stdint.h>

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void Process_OnTaskSwitch(uint32_t previous_task, uint32_t next_task);
extern void Process_OnTaskExit(uint32_t task_id);
extern __attribute__((noreturn)) void Process_ExitKernelCurrent(int32_t exit_code);

static Task tasks[TASK_MAX];
static uint32_t task_count;
static uint32_t current_task;
static void (*kernel_task_entries[TASK_MAX])(void);

#define TASK_IDLE_SLOT (TASK_MAX - 1u)
static uint64_t idle_task_stack[512] __attribute__((aligned(16)));

static void build_kernel_task_frame(uint64_t **stack_ptr,
                                    void (*entry)(void));

static uint64_t task_irq_save_disable(void) {
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
  return flags;
}

static void task_irq_restore(uint64_t flags) {
  if (flags & (1ull << 9))
    __asm__ volatile("sti" ::: "memory");
}

#ifdef VEXOS_CI_INPUT_TEST
static volatile uint64_t ci_runtime_bad_frame_cs;
static volatile int ci_runtime_setup_ok;
static volatile uint64_t ci_runtime_count_a;
static volatile uint64_t ci_runtime_count_b;
#endif

static __attribute__((noreturn)) void task_entry_trampoline(void) {
  uint32_t task_id = current_task;
  void (*entry)(void) =
      task_id < TASK_MAX ? kernel_task_entries[task_id] : NULL;
  if (!entry)
    Process_ExitKernelCurrent(-1);

  entry();

  /* A kernel task that returns has completed normally. The process manager
     marks it exited and the scheduler switches away from its stack. */
  Process_ExitKernelCurrent(0);
}

static __attribute__((noreturn)) void task_idle_entry(void) {
  for (;;) {
    /* The idle context is the scheduler's safe destination when every
       ordinary task is BLOCKED or ZOMBIE. STI;HLT closes the lost-wakeup gap. */
    __asm__ volatile("sti; hlt" ::: "memory");
  }
}

static void task_activate(uint32_t id) {
  if (id >= task_count)
    return;

  Task *task = &tasks[id];
  if (task->address_space)
    VMM_ActivateAddressSpace(task->address_space);
  else
    VMM_Activate();

  /* RSP0 is consulted by the CPU only when entering ring 0 from ring 3.
     Keep it associated with the task that is about to run. */
  if (task->kernel_stack_top)
    TSS_Load(task->kernel_stack_top);
}

void Task_Init(void) {
  for (uint32_t i = 0; i < TASK_MAX; ++i) {
    tasks[i].rsp = 0;
    tasks[i].kernel_stack_top = 0;
    tasks[i].address_space = NULL;
    tasks[i].state = TASK_UNUSED;
    tasks[i].user_mode = 0;
    kernel_task_entries[i] = NULL;
  }

  /* Keep the idle task permanently reserved in the final scheduler slot.
     Ordinary task allocation uses slots 1..TASK_IDLE_SLOT-1. */
  task_count = TASK_MAX;
  current_task = 0;
  tasks[0].state = TASK_RUNNING;

  uint64_t *idle_stack = &idle_task_stack[512];
  kernel_task_entries[TASK_IDLE_SLOT] = task_idle_entry;
  build_kernel_task_frame(&idle_stack, task_entry_trampoline);
  tasks[TASK_IDLE_SLOT].rsp = (uint64_t)(uintptr_t)idle_stack;
  tasks[TASK_IDLE_SLOT].kernel_stack_top =
      (uint64_t)(uintptr_t)&idle_task_stack[512];
  tasks[TASK_IDLE_SLOT].address_space = NULL;
  tasks[TASK_IDLE_SLOT].user_mode = 0;
  tasks[TASK_IDLE_SLOT].state = TASK_READY;
}

static int32_t find_free_task_slot(void) {
  for (uint32_t i = 1; i < TASK_IDLE_SLOT; ++i) {
    if (tasks[i].state == TASK_UNUSED)
      return (int32_t)i;
  }
  return -1;
}

static void build_kernel_task_frame(uint64_t **stack_ptr,
                                    void (*entry)(void)) {
  uint64_t *stack = *stack_ptr;

  /* In 64-bit long mode IRETQ always restores all five slots, even
     for a same-CPL return: RIP, CS, RFLAGS, RSP, SS. The saved RSP points
     at the fake C return address placed at the top of this task's stack. */
  *(--stack) = 0;  // Fake C return address at stack_top - 8.
  uint64_t initial_rsp = (uint64_t)(uintptr_t)stack;

  *(--stack) = 0x10; // Kernel SS.
  *(--stack) = initial_rsp;
  *(--stack) = 0x202; // RFLAGS.
  *(--stack) = 0x08;  // Kernel CS.
  *(--stack) = (uint64_t)(uintptr_t)entry;

  /* Match PUSH_GPRS in interrupt_stubs.s: RAX..R15 are pushed in order. */
  for (uint32_t i = 0; i < 15; ++i)
    *(--stack) = 0;

  *stack_ptr = stack;
}

int32_t Task_Create(void (*entry)(void), void *stack_top) {
  uint64_t irq_flags = task_irq_save_disable();
  if (!entry || !stack_top) {
    task_irq_restore(irq_flags);
    return -1;
  }

  int32_t slot = find_free_task_slot();
  if (slot < 0) {
    task_irq_restore(irq_flags);
    return -1;
  }

  uint64_t *stack = (uint64_t *)stack_top;
  kernel_task_entries[slot] = entry;
  build_kernel_task_frame(&stack, task_entry_trampoline);

  tasks[slot].rsp = (uint64_t)(uintptr_t)stack;
  tasks[slot].kernel_stack_top = (uint64_t)(uintptr_t)stack_top;
  tasks[slot].address_space = NULL;
  tasks[slot].user_mode = 0;
  tasks[slot].state = TASK_READY;

  task_irq_restore(irq_flags);
  return slot;
}

int32_t Task_CreateUser(AddressSpace *space, uint64_t entry,
                        uint64_t user_stack_top, void *kernel_stack_top) {
  uint64_t irq_flags = task_irq_save_disable();
  if (!space || !entry || !user_stack_top || !kernel_stack_top) {
    task_irq_restore(irq_flags);
    return -1;
  }
  if (!VMM_AddressSpaceIsUserRange(space, (const void *)(uintptr_t)entry, 1, 0) ||
      user_stack_top < 1u ||
      !VMM_AddressSpaceIsUserRange(space,
          (const void *)(uintptr_t)(user_stack_top - 1u), 1, 1)) {
    task_irq_restore(irq_flags);
    return -1;
  }

  int32_t slot = find_free_task_slot();
  if (slot < 0) {
    task_irq_restore(irq_flags);
    return -1;
  }

  uint64_t *stack = (uint64_t *)kernel_stack_top;

  /* User-mode IRET frame: RIP, CS, RFLAGS, user RSP, SS. */
  *(--stack) = 0x23;
  *(--stack) = user_stack_top;
  *(--stack) = 0x202;
  *(--stack) = 0x1B;
  *(--stack) = entry;
  for (uint32_t i = 0; i < 15; ++i)
    *(--stack) = 0;

  tasks[slot].rsp = (uint64_t)(uintptr_t)stack;
  tasks[slot].kernel_stack_top = (uint64_t)(uintptr_t)kernel_stack_top;
  tasks[slot].address_space = space;
  tasks[slot].user_mode = 1;
  kernel_task_entries[slot] = NULL;
  tasks[slot].state = TASK_READY;

  task_irq_restore(irq_flags);
  return slot;
}

int Task_SetTaskContext(uint32_t task_id, AddressSpace *space,
                        uint64_t kernel_stack_top, int user_mode) {
  uint64_t irq_flags = task_irq_save_disable();
  if (task_id >= task_count || tasks[task_id].state == TASK_UNUSED ||
      tasks[task_id].state == TASK_ZOMBIE) {
    task_irq_restore(irq_flags);
    return 0;
  }

  tasks[task_id].address_space = space;
  tasks[task_id].kernel_stack_top = kernel_stack_top;
  tasks[task_id].user_mode = user_mode ? 1u : 0u;
  if (task_id == current_task)
    task_activate(task_id);
  task_irq_restore(irq_flags);
  return 1;
}

int Task_SetCurrentContext(AddressSpace *space, uint64_t kernel_stack_top,
                           int user_mode) {
  return Task_SetTaskContext(current_task, space, kernel_stack_top, user_mode);
}

uint32_t Task_GetCurrentId(void) {
  return current_task;
}

uint64_t Task_GetTaskKernelStackTop(uint32_t task_id) {
  if (task_id >= task_count || tasks[task_id].state == TASK_UNUSED)
    return 0;
  return tasks[task_id].kernel_stack_top;
}

void Task_BlockCurrent(void) {
  uint64_t irq_flags = task_irq_save_disable();
  if (current_task < task_count && tasks[current_task].state == TASK_RUNNING)
    tasks[current_task].state = TASK_BLOCKED;
  task_irq_restore(irq_flags);
}

void Task_MakeReady(uint32_t task_id) {
  uint64_t irq_flags = task_irq_save_disable();
  if (task_id < task_count && tasks[task_id].state == TASK_BLOCKED)
    tasks[task_id].state = TASK_READY;
  task_irq_restore(irq_flags);
}

int Task_Reap(uint32_t task_id) {
  uint64_t irq_flags = task_irq_save_disable();
  if (task_id == current_task || task_id >= task_count ||
      tasks[task_id].state != TASK_ZOMBIE) {
    task_irq_restore(irq_flags);
    return 0;
  }

  tasks[task_id].rsp = 0;
  tasks[task_id].kernel_stack_top = 0;
  tasks[task_id].address_space = NULL;
  tasks[task_id].user_mode = 0;
  kernel_task_entries[task_id] = NULL;
  tasks[task_id].state = TASK_UNUSED;
  task_irq_restore(irq_flags);
  return 1;
}

int Task_Discard(uint32_t task_id) {
  uint64_t irq_flags = task_irq_save_disable();
  if (task_id == current_task || task_id >= task_count ||
      tasks[task_id].state != TASK_READY) {
    task_irq_restore(irq_flags);
    return 0;
  }

  tasks[task_id].rsp = 0;
  tasks[task_id].kernel_stack_top = 0;
  tasks[task_id].address_space = NULL;
  tasks[task_id].user_mode = 0;
  kernel_task_entries[task_id] = NULL;
  tasks[task_id].state = TASK_UNUSED;
  task_irq_restore(irq_flags);
  return 1;
}

uint64_t Task_Schedule(uint64_t current_rsp) {
  uint32_t previous = current_task;
  tasks[previous].rsp = current_rsp;

  if (tasks[previous].state == TASK_RUNNING)
    tasks[previous].state = TASK_READY;

  /* Ordinary work always outranks idle. If every ordinary task is blocked
     or exited, the reserved idle frame is the only legal scheduler fallback. */
  for (uint32_t pass = 0; pass < 2; ++pass) {
    for (uint32_t offset = 1; offset <= task_count; ++offset) {
      uint32_t next = (previous + offset) % task_count;
      if ((next == TASK_IDLE_SLOT) != (pass == 1u) ||
          tasks[next].state != TASK_READY)
        continue;

      current_task = next;
      tasks[next].state = TASK_RUNNING;
#ifdef VEXOS_CI_INPUT_TEST
      if (ci_runtime_setup_ok && tasks[next].rsp) {
        const uint64_t *frame = (const uint64_t *)(uintptr_t)tasks[next].rsp;
        if (frame[16] != 0x08u && frame[16] != 0x1Bu) {
          ci_runtime_bad_frame_cs = frame[16];
          current_task = previous;
          if (tasks[previous].state != TASK_BLOCKED &&
              tasks[previous].state != TASK_ZOMBIE)
            tasks[previous].state = TASK_RUNNING;
          return current_rsp;
        }
      }
#endif
      Process_OnTaskSwitch(previous, next);
      task_activate(next);
      return tasks[next].rsp;
    }
  }

  /* Task_Init permanently installs a ready idle frame, so reaching this
     means scheduler state has been corrupted. Never resume a blocked task. */
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}

uint64_t Task_Exit(uint64_t current_rsp) {
  (void)current_rsp;
  uint32_t previous = current_task;
  tasks[previous].state = TASK_ZOMBIE;
  Process_OnTaskExit(previous);

  for (uint32_t pass = 0; pass < 2; ++pass) {
    for (uint32_t offset = 1; offset <= task_count; ++offset) {
      uint32_t next = (previous + offset) % task_count;
      if ((next == TASK_IDLE_SLOT) != (pass == 1u) ||
          tasks[next].state != TASK_READY)
        continue;

      current_task = next;
      tasks[next].state = TASK_RUNNING;
      Process_OnTaskSwitch(previous, next);
      task_activate(next);
      return tasks[next].rsp;
    }
  }

  /* The reserved idle frame should always be runnable. */
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}

void Task_Yield(void) {
  __asm__ volatile("int $0x81" ::: "memory");
}

__attribute__((noreturn)) void Task_ExitCurrent(void) {
  /* Keep the transition to ZOMBIE and the software reschedule indivisible
     from the PIT scheduler's point of view. */
  __asm__ volatile("cli" ::: "memory");
  if (current_task >= task_count)
    for (;;) __asm__ volatile("cli; hlt");

  tasks[current_task].state = TASK_ZOMBIE;
  Process_OnTaskExit(current_task);
  Task_Yield();

  /* A successfully exited task must never be resumed. */
  for (;;) __asm__ volatile("cli; hlt");
}

#ifdef VEXOS_CI_INPUT_TEST
static void ci_task_entry(void) {}

int Task_CI_SelfTest(void) {
  static uint64_t test_stack[64] __attribute__((aligned(16)));

  Task_Init();
  if (Task_Create(NULL, &test_stack[64]) >= 0)
    return 0;
  if (Task_Create(ci_task_entry, NULL) >= 0)
    return 0;
  if (task_count != TASK_MAX ||
      tasks[TASK_IDLE_SLOT].state != TASK_READY)
    return 0;

  int32_t id = Task_Create(ci_task_entry, &test_stack[64]);
  if (id != 1 || task_count != TASK_MAX)
    return 0;

  uint64_t next_rsp = Task_Schedule(0x1111u);
  if (!next_rsp || (next_rsp & 15u) != 8u)
    return 0;

  const uint64_t *frame = (const uint64_t *)(uintptr_t)next_rsp;
  for (uint32_t i = 0; i < 15u; i++) {
    if (frame[i] != 0)
      return 0;
  }
  if (frame[15] != (uint64_t)(uintptr_t)task_entry_trampoline ||
      frame[16] != 0x08u || frame[17] != 0x202u ||
      frame[18] != (uint64_t)(uintptr_t)&test_stack[63] ||
      frame[19] != 0x10u)
    return 0;

  if (Task_Schedule(0x2222u) != 0x1111u ||
      Task_GetCurrentId() != 0u)
    return 0;

  /* A blocked final ordinary task must yield to idle, never resume itself. */
  tasks[1].state = TASK_BLOCKED;
  Task_BlockCurrent();
  next_rsp = Task_Schedule(0x3333u);
  if (Task_GetCurrentId() != TASK_IDLE_SLOT ||
      next_rsp != tasks[TASK_IDLE_SLOT].rsp ||
      tasks[TASK_IDLE_SLOT].state != TASK_RUNNING)
    return 0;
  Task_MakeReady(0u);
  if (Task_Schedule(0x4444u) != 0x3333u ||
      Task_GetCurrentId() != 0u)
    return 0;

  Task_Init();
  return 1;
}

static void ci_runtime_task_a(void) {
  for (;;) {
    ci_runtime_count_a++;
    __asm__ volatile("pause");
  }
}

static void ci_runtime_task_b(void) {
  for (;;) {
    ci_runtime_count_b++;
    __asm__ volatile("pause");
  }
}

void Task_CI_StartRuntimeTest(void) {
  ci_runtime_count_a = 0;
  ci_runtime_count_b = 0;
  ci_runtime_setup_ok = 0;
  ci_runtime_bad_frame_cs = 0;

  void *stack_a = kmalloc(4096u);
  void *stack_b = kmalloc(4096u);
  if (!stack_a || !stack_b) {
    if (stack_a) kfree(stack_a);
    if (stack_b) kfree(stack_b);
    return;
  }

  int32_t id_a = Task_Create(ci_runtime_task_a, (uint8_t *)stack_a + 4096u);
  int32_t id_b = Task_Create(ci_runtime_task_b, (uint8_t *)stack_b + 4096u);
  ci_runtime_setup_ok = id_a == 1 && id_b == 2 && task_count == TASK_MAX;

  if (ci_runtime_setup_ok) {
    const uint64_t *frame_a = (const uint64_t *)(uintptr_t)tasks[1].rsp;
    const uint64_t *frame_b = (const uint64_t *)(uintptr_t)tasks[2].rsp;
    ci_runtime_setup_ok =
        frame_a[15] == (uint64_t)(uintptr_t)task_entry_trampoline &&
        frame_a[16] == 0x08u && frame_a[17] == 0x202u &&
        frame_a[18] == (uint64_t)(uintptr_t)((uint8_t *)stack_a + 4096u - sizeof(uint64_t)) &&
        frame_a[19] == 0x10u &&
        frame_b[15] == (uint64_t)(uintptr_t)task_entry_trampoline &&
        frame_b[16] == 0x08u && frame_b[17] == 0x202u &&
        frame_b[18] == (uint64_t)(uintptr_t)((uint8_t *)stack_b + 4096u - sizeof(uint64_t)) &&
        frame_b[19] == 0x10u;
  }

  extern void serial_print(const char *str);
  serial_print(ci_runtime_setup_ok
      ? "[CI] Runtime task IRET frames validated\\n"
      : "[CI] Runtime task IRET frame validation failed\\n");
  if (!ci_runtime_setup_ok)
    Task_Init();
}

int Task_CI_RuntimeTestPassed(void) {
  return ci_runtime_setup_ok &&
         ci_runtime_count_a >= 100u && ci_runtime_count_b >= 100u;
}

uint64_t Task_CI_RuntimeBadFrameCS(void) {
  return ci_runtime_bad_frame_cs;
}
#endif
