#include "../include/task.h"
#include "../include/heap.h"
#include <stddef.h>

extern void context_switch(uint64_t* old_rsp, uint64_t new_rsp);

static Task tasks[10];
static int task_count = 0;
static int current_task = 0;

void Task_Init() {
    // Current execution becomes Task 0
    task_count = 1;
    current_task = 0;
}

void Task_Create(void (*entry)(), void* stack_top) {
    if (task_count >= 10) return;
    
    uint64_t* stack = (uint64_t*)stack_top;
    
    // Interrupt Frame
    *(--stack) = 0x10;             // SS
    *(--stack) = (uint64_t)stack_top; // RSP
    *(--stack) = 0x202;            // RFLAGS (Interrupts enabled)
    *(--stack) = 0x08;             // CS
    *(--stack) = (uint64_t)entry;  // RIP

    // Push dummy registers for the stub (15 registers)
    for(int i=0; i<15; i++) {
        *(--stack) = 0;
    }
    
    tasks[task_count].rsp = (uint64_t)stack;
    task_count++;
}

uint64_t Task_Schedule(uint64_t current_rsp) {
    tasks[current_task].rsp = current_rsp;
    current_task = (current_task + 1) % task_count;
    return tasks[current_task].rsp;
}

