#ifndef TASK_H
#define TASK_H

#include <stdint.h>

#define TASK_MAX 16u

struct AddressSpace;
typedef struct AddressSpace AddressSpace;

typedef enum {
  TASK_UNUSED = 0,
  TASK_READY,
  TASK_RUNNING,
  TASK_BLOCKED,
  TASK_ZOMBIE
} TaskState;

typedef struct {
  uint64_t rsp;
  uint64_t kernel_stack_top;
  AddressSpace *address_space;
  uint8_t state;
  uint8_t user_mode;
} Task;

void Task_Init(void);
int32_t Task_Create(void (*entry)(void), void *stack_top);
int32_t Task_CreateUser(AddressSpace *space, uint64_t entry,
                        uint64_t user_stack_top, void *kernel_stack_top);
int Task_SetTaskContext(uint32_t task_id, AddressSpace *space,
                        uint64_t kernel_stack_top, int user_mode);
int Task_SetCurrentContext(AddressSpace *space, uint64_t kernel_stack_top,
                           int user_mode);
uint32_t Task_GetCurrentId(void);
uint64_t Task_GetTaskKernelStackTop(uint32_t task_id);
void Task_BlockCurrent(void);
void Task_MakeReady(uint32_t task_id);
int Task_Reap(uint32_t task_id);
int Task_Discard(uint32_t task_id);
void Task_Yield(void);
uint64_t Task_Schedule(uint64_t current_rsp);
uint64_t Task_Exit(uint64_t current_rsp);
void Task_ExitCurrent(void);

#endif
