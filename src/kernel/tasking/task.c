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
