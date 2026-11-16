#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "task.h"

typedef TaskState ProcessState;

/* Kernel process API. A caller supplies a kernel entry point and owned stack;
   an optional address space is activated whenever the process is scheduled. */
void Process_Init(void);
uint64_t Process_CreateKernel(const char *name, void (*entry)(void),
                              void *kernel_stack_top, AddressSpace *space);
void Process_AttachCurrentAddressSpace(AddressSpace *space);
void Process_SetCurrentKernelStack(uint64_t stack_top);
uint64_t Process_CurrentPid(void);
ProcessState Process_GetState(uint64_t pid);
void Process_Yield(void);
uint64_t Process_Exit(uint64_t interrupt_frame);

#endif
