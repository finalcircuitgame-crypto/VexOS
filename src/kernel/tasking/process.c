#include "../../include/process.h"

void Process_Init(void) {
  Task_Init();
}

uint64_t Process_CreateKernel(const char *name, void (*entry)(void),
                              void *kernel_stack_top, AddressSpace *space) {
  return Task_CreateProcess(name, entry, kernel_stack_top, space);
}

void Process_AttachCurrentAddressSpace(AddressSpace *space) {
  Task_SetCurrentAddressSpace(space);
}

void Process_SetCurrentKernelStack(uint64_t stack_top) {
  Task_SetCurrentKernelStack(stack_top);
}

uint64_t Process_CurrentPid(void) {
  return Task_CurrentPid();
}

ProcessState Process_GetState(uint64_t pid) {
  return Task_GetState(pid);
}

void Process_Yield(void) {
  Task_Yield();
}

uint64_t Process_Exit(uint64_t interrupt_frame) {
  return Task_Exit(interrupt_frame);
}
