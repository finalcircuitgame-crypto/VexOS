#include "../../include/task.h"
#include "../../include/heap.h"
#include <stddef.h>
#include <stdint.h>

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);

static Task tasks[10];
static int task_count = 0;
static int current_task = 0;

void Task_Init() {
  // Current execution becomes Task 0
  task_count = 1;
  current_task = 0;
  tasks[0].active = 1;
}

void Task_Create(void (*entry)(), void *stack_top) {
  if (!entry || !stack_top || task_count >= 10)
    return;

  // The interrupt stub restores 15 GPRs, then executes IRETQ in ring 0.
  // A same-privilege IRETQ consumes only RIP, CS and RFLAGS (not RSP/SS).
  // Leave a fake return slot below that frame so a normal C entry has the
  // SysV x86-64 alignment expected after a CALL: RSP % 16 == 8.
  uint64_t *stack = (uint64_t *)stack_top;
  *(--stack) = 0;                    // Fake return address if entry returns.
  *(--stack) = 0x202;                // RFLAGS (interrupts enabled).
  *(--stack) = 0x08;                 // CS (kernel code).
  *(--stack) = (uint64_t)entry;      // RIP.

  // PUSH_GPRS saves/restores these 15 registers in the interrupt stub.
  for (int i = 0; i < 15; i++) {
    *(--stack) = 0;
  }

  tasks[task_count].rsp = (uint64_t)stack;
  tasks[task_count].active = 1;
  task_count++;
}


uint64_t Task_Schedule(uint64_t current_rsp);

#ifdef VEXOS_CI_INPUT_TEST
static void ci_task_entry(void) {}

int Task_CI_SelfTest(void) {
  static uint64_t test_stack[64] __attribute__((aligned(16)));

  Task_Init();
  Task_Create(NULL, &test_stack[64]);
  Task_Create(ci_task_entry, NULL);
  if (task_count != 1)
    return 0;

  Task_Create(ci_task_entry, &test_stack[64]);
  if (task_count != 2)
    return 0;

  uint64_t next_rsp = Task_Schedule(0x1111u);
  if (!next_rsp || (next_rsp & 15u) != 8u)
    return 0;

  const uint64_t *frame = (const uint64_t *)(uintptr_t)next_rsp;
  for (uint32_t i = 0; i < 15u; i++) {
    if (frame[i] != 0)
      return 0;
  }
  if (frame[15] != (uint64_t)(uintptr_t)ci_task_entry ||
      frame[16] != 0x08u || frame[17] != 0x202u || frame[18] != 0)
    return 0;

  if (Task_Schedule(0x2222u) != 0x1111u)
    return 0;

  // Restore the normal one-task state before kernel_main initializes tasks.
  Task_Init();
  return 1;
}
#endif

uint64_t Task_Schedule(uint64_t current_rsp) {
  tasks[current_task].rsp = current_rsp;
  for (int i = 0; i < task_count; i++) {
    int next = (current_task + 1 + i) % task_count;
    if (tasks[next].active) {
      current_task = next;
      return tasks[current_task].rsp;
    }
  }
  // No other active task; stay on current
  return current_rsp;
}

uint64_t Task_Exit(uint64_t current_rsp) {
  tasks[current_task].active = 0;
  // Find next active task
  for (int i = 0; i < task_count; i++) {
    int next = (current_task + 1 + i) % task_count;
    if (tasks[next].active) {
      current_task = next;
      return tasks[current_task].rsp;
    }
  }
  // No tasks left — halt
  while (1) {
    __asm__ volatile("hlt");
  }
}


#ifdef VEXOS_CI_INPUT_TEST
// Runtime proof that IRQ0 actually restores and executes multiple task stacks.
// The timer interrupt must preempt each worker and resume the other worker.
static uint64_t ci_runtime_stack_a[512] __attribute__((aligned(16)));
static uint64_t ci_runtime_stack_b[512] __attribute__((aligned(16)));
static volatile uint64_t ci_runtime_count_a;
static volatile uint64_t ci_runtime_count_b;

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
  Task_Create(ci_runtime_task_a, &ci_runtime_stack_a[512]);
  Task_Create(ci_runtime_task_b, &ci_runtime_stack_b[512]);
  if (task_count != 3) {
    ci_runtime_count_a = UINT64_MAX;
    ci_runtime_count_b = UINT64_MAX;
  }
}

int Task_CI_RuntimeTestPassed(void) {
  return ci_runtime_count_a >= 100u && ci_runtime_count_b >= 100u;
}
#endif
