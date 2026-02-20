#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

void Heap_Init(void* addr, size_t size);
void* kmalloc(size_t size);
void kfree(void* ptr);

#endif
