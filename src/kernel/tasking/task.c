#include "../../include/task.h"
#include "../../include/heap.h"
#include "../../include/gdt.h"
#include <stddef.h>
#include <stdint.h>

#define MAX_TASKS 32u

static Task tasks[MAX_TASKS];
static uint32_t task_count;
static uint32_t current_task;
static uint64_t next_pid;

#ifdef VEXOS_CI_INPUT_TEST
static volatile uint64_t ci_runtime_bad_frame_cs;
static volatile int ci_runtime_setup_ok;
static volatile int ci_runtime_space_a_ok;
static volatile int ci_runtime_space_b_ok;
static volatile uint64_t ci_runtime_count_a;
static volatile uint64_t ci_runtime_count_b;
static AddressSpace *ci_runtime_space_a;
static AddressSpace *ci_runtime_space_b;
#endif

static void copy_name(char dst[TASK_NAME_MAX], const char *src) {
  uint32_t i = 0;
  if (!src || !src[0]) src = "process";
  for (; i + 1u < TASK_NAME_MAX && src[i]; ++i) dst[i] = src[i];
  dst[i] = '\0';
  for (++i; i < TASK_NAME_MAX; ++i) dst[i] = '\0';
}

void Task_Init(void) {
  for (uint32_t i = 0; i < MAX_TASKS; ++i) {
    tasks[i].rsp = 0;
    tasks[i].pid = 0;
    tasks[i].parent_pid = 0;
    tasks[i].kernel_stack_top = 0;
    tasks[i].address_space = NULL;
    tasks[i].state = TASK_UNUSED;
    tasks[i].name[0] = '\0';
  }
  task_count = 1;
  current_task = 0;
  next_pid = 2;
  tasks[0].pid = 1;
  tasks[0].state = TASK_RUNNING;
  copy_name(tasks[0].name, "kernel");
}

uint64_t Task_CreateProcess(const char *name, void (*entry)(void),
                            void *stack_top, AddressSpace *space) {
  if (!entry || !stack_top) return 0;

  uint32_t slot = MAX_TASKS;
  for (uint32_t i = 1; i < task_count; ++i) {
    if (tasks[i].state == TASK_UNUSED || tasks[i].state == TASK_EXITED) {
      slot = i;
      break;
    }
  }
  if (slot == MAX_TASKS) {
    if (task_count >= MAX_TASKS) return 0;
    slot = task_count++;
  }

  /* irq0_stub restores 15 GPRs, then IRETQ consumes the five-word
     same-privilege frame (RIP, CS, RFLAGS, RSP, SS). */
  uint64_t *stack = (uint64_t *)stack_top;
  *(--stack) = 0; /* harmless fake return slot */
  uint64_t initial_rsp = (uint64_t)(uintptr_t)stack;
  *(--stack) = 0x10;        /* SS */
  *(--stack) = initial_rsp; /* RSP */
  *(--stack) = 0x202;       /* RFLAGS */
  *(--stack) = 0x08;        /* CS */
  *(--stack) = (uint64_t)(uintptr_t)entry; /* RIP */
  for (uint32_t i = 0; i < 15u; ++i) *(--stack) = 0;

  Task *task = &tasks[slot];
  task->rsp = (uint64_t)(uintptr_t)stack;
  task->pid = next_pid++;
  if (next_pid == 0) next_pid = 2;
  task->parent_pid = tasks[current_task].pid;
  task->kernel_stack_top = (uint64_t)(uintptr_t)stack_top;
  task->address_space = space;
  task->state = TASK_RUNNABLE;
  copy_name(task->name, name);
  return task->pid;
}

void Task_Create(void (*entry)(void), void *stack_top) {
  (void)Task_CreateProcess("kernel-worker", entry, stack_top, NULL);
}

void Task_SetCurrentAddressSpace(AddressSpace *space) {
  tasks[current_task].address_space = space;
}

void Task_SetCurrentKernelStack(uint64_t stack_top) {
  tasks[current_task].kernel_stack_top = stack_top;
  if (stack_top) TSS_Load(stack_top);
}

uint64_t Task_CurrentPid(void) {
  return tasks[current_task].pid;
}

TaskState Task_GetState(uint64_t pid) {
  for (uint32_t i = 0; i < task_count; ++i)
    if (tasks[i].pid == pid) return tasks[i].state;
  return TASK_UNUSED;
}

static void activate_task(uint32_t index) {
  Task *task = &tasks[index];
  if (task->address_space) VMM_ActivateAddressSpace(task->address_space);
  else VMM_Activate();
  if (task->kernel_stack_top) TSS_Load(task->kernel_stack_top);
}

uint64_t Task_Schedule(uint64_t current_rsp) {
  Task *current = &tasks[current_task];
  current->rsp = current_rsp;
  if (current->state == TASK_RUNNING) current->state = TASK_RUNNABLE;

  for (uint32_t offset = 1; offset <= task_count; ++offset) {
    uint32_t next = (current_task + offset) % task_count;
    if (tasks[next].state != TASK_RUNNABLE) continue;
#ifdef VEXOS_CI_INPUT_TEST
    if (ci_runtime_setup_ok && tasks[next].rsp) {
      const uint64_t *frame = (const uint64_t *)(uintptr_t)tasks[next].rsp;
      if (frame[16] != 0x08u && frame[16] != 0x1Bu) {
        ci_runtime_bad_frame_cs = frame[16];
        current->state = TASK_RUNNING;
        return current_rsp;
      }
    }
#endif
    current_task = next;
    tasks[next].state = TASK_RUNNING;
    activate_task(next);
    return tasks[next].rsp;
  }

  current->state = TASK_RUNNING;
  activate_task(current_task);
  return current_rsp;
}

uint64_t Task_Exit(uint64_t current_rsp) {
  (void)current_rsp;
  tasks[current_task].state = TASK_EXITED;
  tasks[current_task].address_space = NULL;
  tasks[current_task].kernel_stack_top = 0;

  for (uint32_t offset = 1; offset <= task_count; ++offset) {
    uint32_t next = (current_task + offset) % task_count;
    if (tasks[next].state != TASK_RUNNABLE) continue;
    current_task = next;
    tasks[next].state = TASK_RUNNING;
    activate_task(next);
    return tasks[next].rsp;
  }

  VMM_Activate();
  for (;;) __asm__ volatile("cli; hlt");
}

void Task_Yield(void) {
  __asm__ volatile("int $0x20" ::: "memory");
}

#ifdef VEXOS_CI_INPUT_TEST
static void ci_task_entry(void) {}

int Task_CI_SelfTest(void) {
  static uint64_t test_stack[64] __attribute__((aligned(16)));
  Task_Init();
  (void)Task_CreateProcess("invalid", NULL, &test_stack[64], NULL);
  (void)Task_CreateProcess("invalid", ci_task_entry, NULL, NULL);
  if (task_count != 1u) return 0;

  uint64_t pid = Task_CreateProcess("frame-test", ci_task_entry,
                                    &test_stack[64], NULL);
  if (!pid || task_count != 2u || Task_GetState(pid) != TASK_RUNNABLE)
    return 0;

  uint64_t next_rsp = Task_Schedule(0x1111u);
  if (!next_rsp || (next_rsp & 15u) != 8u) return 0;
  const uint64_t *frame = (const uint64_t *)(uintptr_t)next_rsp;
  for (uint32_t i = 0; i < 15u; ++i)
    if (frame[i] != 0) return 0;
  if (frame[15] != (uint64_t)(uintptr_t)ci_task_entry ||
      frame[16] != 0x08u || frame[17] != 0x202u ||
      frame[18] != (uint64_t)(uintptr_t)&test_stack[63] ||
      frame[19] != 0x10u) return 0;
  if (Task_Schedule(0x2222u) != 0x1111u) return 0;
  Task_Init();
  return 1;
}

static void ci_runtime_task_a(void) {
  for (;;) {
    if (VMM_GetActiveAddressSpace() != ci_runtime_space_a)
      ci_runtime_space_a_ok = 0;
    ci_runtime_count_a++;
    __asm__ volatile("pause");
  }
}

static void ci_runtime_task_b(void) {
  for (;;) {
    if (VMM_GetActiveAddressSpace() != ci_runtime_space_b)
      ci_runtime_space_b_ok = 0;
    ci_runtime_count_b++;
    __asm__ volatile("pause");
  }
}

void Task_CI_StartRuntimeTest(void) {
  ci_runtime_count_a = ci_runtime_count_b = 0;
  ci_runtime_setup_ok = 0;
  ci_runtime_space_a_ok = ci_runtime_space_b_ok = 1;
  ci_runtime_bad_frame_cs = 0;

  void *stack_a = kmalloc(4096u);
  void *stack_b = kmalloc(4096u);
  ci_runtime_space_a = VMM_AddressSpaceCreate();
  ci_runtime_space_b = VMM_AddressSpaceCreate();
  if (!stack_a || !stack_b || !ci_runtime_space_a || !ci_runtime_space_b) {
    if (stack_a) kfree(stack_a);
    if (stack_b) kfree(stack_b);
    return;
  }

  uint64_t pid_a = Task_CreateProcess("ci-space-a", ci_runtime_task_a,
      (uint8_t *)stack_a + 4096u, ci_runtime_space_a);
  uint64_t pid_b = Task_CreateProcess("ci-space-b", ci_runtime_task_b,
      (uint8_t *)stack_b + 4096u, ci_runtime_space_b);
  ci_runtime_setup_ok = pid_a && pid_b && task_count == 3u;

  if (ci_runtime_setup_ok) {
    const uint64_t *a = (const uint64_t *)(uintptr_t)tasks[1].rsp;
    const uint64_t *b = (const uint64_t *)(uintptr_t)tasks[2].rsp;
    ci_runtime_setup_ok =
      a[15] == (uint64_t)(uintptr_t)ci_runtime_task_a &&
      b[15] == (uint64_t)(uintptr_t)ci_runtime_task_b &&
      a[16] == 0x08u && b[16] == 0x08u &&
      a[17] == 0x202u && b[17] == 0x202u;
  }

  extern void serial_print(const char *str);
  serial_print(ci_runtime_setup_ok
      ? "[CI] Runtime task IRET frames validated\\n"
      : "[CI] Runtime task IRET frame validation failed\\n");
  if (ci_runtime_setup_ok)
    serial_print("[CI] Process control blocks and private CR3 fixtures created\\n");
  if (!ci_runtime_setup_ok) Task_Init();
}

int Task_CI_RuntimeTestPassed(void) {
  return ci_runtime_setup_ok && ci_runtime_space_a_ok &&
         ci_runtime_space_b_ok && ci_runtime_count_a >= 100u &&
         ci_runtime_count_b >= 100u;
}

uint64_t Task_CI_RuntimeBadFrameCS(void) {
  return ci_runtime_bad_frame_cs;
}
#endif
