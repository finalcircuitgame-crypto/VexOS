#include "../include/heap.h"
#include <stddef.h>

typedef struct HeapNode {
    size_t size;
    struct HeapNode* next;
    int free;
} HeapNode;

static HeapNode* head = NULL;

void Heap_Init(void* addr, size_t size) {
    head = (HeapNode*)addr;
    head->size = size - sizeof(HeapNode);
    head->next = NULL;
    head->free = 1;
}

void* kmalloc(size_t size) {
    HeapNode* current = head;
    while (current) {
        if (current->free && current->size >= size) {
            // Can we split?
            if (current->size > size + sizeof(HeapNode) + 16) {
                HeapNode* next = (HeapNode*)((char*)current + sizeof(HeapNode) + size);
                next->size = current->size - size - sizeof(HeapNode);
                next->next = current->next;
                next->free = 1;
                
                current->size = size;
                current->next = next;
            }
            current->free = 0;
            return (void*)((char*)current + sizeof(HeapNode));
        }
        current = current->next;
    }
    return NULL;
}

void kfree(void* ptr) {
    if (!ptr) return;
    HeapNode* node = (HeapNode*)((char*)ptr - sizeof(HeapNode));
    node->free = 1;
    
    // Simple merging
    HeapNode* current = head;
    while (current) {
        if (current->free && current->next && current->next->free) {
            current->size += current->next->size + sizeof(HeapNode);
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}
