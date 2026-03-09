#ifndef TASK_H
#define TASK_H

#include <stdint.h>

typedef struct {
  uint64_t rsp;
  uint8_t active;
} Task;

void Task_Init();
void Task_Create(void (*entry)(), void *stack);
void Task_Yield();
uint64_t Task_Exit(uint64_t current_rsp);

#endif
