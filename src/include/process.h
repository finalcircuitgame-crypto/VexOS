#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "task.h"

#define PROCESS_NAME_MAX 32u

typedef uint64_t ProcessId;

typedef enum {
  PROCESS_UNUSED = 0,
  PROCESS_READY,
  PROCESS_RUNNING,
  PROCESS_BLOCKED,
  PROCESS_ZOMBIE
} ProcessState;

typedef struct {
  ProcessId pid;
  ProcessId parent_pid;
  uint32_t task_id;
  ProcessState state;
  int32_t exit_code;
  uint8_t user_mode;
  char name[PROCESS_NAME_MAX];
} ProcessInfo;

void Process_Init(void);
ProcessId Process_RegisterTask(const char *name, uint32_t task_id,
                               void *external_stack_base);
ProcessId Process_CreateKernel(const char *name, void (*entry)(void));
ProcessId Process_CreateUser(const char *name, AddressSpace *space,
                             uint64_t entry, uint64_t user_stack_top);
int Process_BeginUserModeCurrent(AddressSpace *space,
                                 uint64_t kernel_stack_top);
void Process_AttachCurrentAddressSpace(AddressSpace *space);
void Process_SetCurrentKernelStack(uint64_t stack_top);
void Process_DetachCurrentAddressSpace(void);
int Process_IsCurrentUser(void);
ProcessId Process_GetCurrentId(void);
uint32_t Process_GetCount(void);
int Process_GetInfo(ProcessId pid, ProcessInfo *out);
int Process_Wait(ProcessId pid, int32_t *exit_code);
uint64_t Process_ExitCurrent(uint64_t saved_rsp, int32_t exit_code);
int Process_ReapTask(uint32_t task_id);
void Process_OnTaskSwitch(uint32_t previous_task, uint32_t next_task);
void Process_OnTaskExit(uint32_t task_id);

#endif
