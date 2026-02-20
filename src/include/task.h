#ifndef TASK_H
#define TASK_H

#include <stdint.h>

typedef struct {
    uint64_t rsp;
} Task;

void Task_Init();
void Task_Create(void (*entry)(), void* stack);
void Task_Yield();

#endif
