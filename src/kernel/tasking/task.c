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
  if (task_count >= 10)
    return;

  uint64_t *stack = (uint64_t *)stack_top;

  // iretq frame: x86-64 iretq ALWAYS pops all 5 values, even for
  // same-privilege returns. Push them in reverse order (SS first).
  *(--stack) = 0x10;                // SS  (kernel data)
  *(--stack) = (uint64_t)stack_top; // RSP (clean stack for the new task)
  *(--stack) = 0x202;               // RFLAGS (IF=1)
  *(--stack) = 0x08;                // CS  (kernel code)
  *(--stack) = (uint64_t)entry;     // RIP

  // Push dummy registers for PUSH_GPRS (15 registers)
  for (int i = 0; i < 15; i++) {
    *(--stack) = 0;
  }

  tasks[task_count].rsp = (uint64_t)stack;
  tasks[task_count].active = 1;
  task_count++;
}

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
