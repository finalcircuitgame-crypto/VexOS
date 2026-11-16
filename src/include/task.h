#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "vmm.h"

#define TASK_NAME_MAX 32u

typedef enum {
  TASK_UNUSED = 0,
  TASK_RUNNABLE,
  TASK_RUNNING,
  TASK_EXITED
} TaskState;

/* Task is also the kernel's initial process-control-block representation.
   Each scheduled context owns its saved interrupt frame, CR3 and ring-3
   transition stack metadata. */
typedef struct {
  uint64_t rsp;
  uint64_t pid;
  uint64_t parent_pid;
  uint64_t kernel_stack_top;
  AddressSpace *address_space;
  TaskState state;
  char name[TASK_NAME_MAX];
} Task;

void Task_Init(void);
void Task_Create(void (*entry)(void), void *stack_top);
uint64_t Task_CreateProcess(const char *name, void (*entry)(void),
                            void *stack_top, AddressSpace *space);
void Task_SetCurrentAddressSpace(AddressSpace *space);
void Task_SetCurrentKernelStack(uint64_t stack_top);
uint64_t Task_CurrentPid(void);
TaskState Task_GetState(uint64_t pid);
void Task_Yield(void);
uint64_t Task_Exit(uint64_t current_rsp);

#endif
