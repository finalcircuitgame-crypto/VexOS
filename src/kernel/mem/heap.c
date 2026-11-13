#include "../include/heap.h"
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#define HEAP_ALIGNMENT 16u
#define HEAP_ALIGN_UP(value) (((value) + (HEAP_ALIGNMENT - 1u)) & ~(size_t)(HEAP_ALIGNMENT - 1u))

typedef struct __attribute__((aligned(HEAP_ALIGNMENT))) HeapNode {
    size_t size;
    struct HeapNode *next;
    uint32_t free;
    uint32_t reserved;
} HeapNode;

static HeapNode *head;
static uintptr_t heap_begin;
static uintptr_t heap_end;

void Heap_Init(void *addr, size_t size) {
    head = NULL;
    heap_begin = 0;
    heap_end = 0;

    if (!addr || size < sizeof(HeapNode)) return;

    uintptr_t raw_begin = (uintptr_t)addr;
    if (raw_begin > UINTPTR_MAX - (HEAP_ALIGNMENT - 1u)) return;
    uintptr_t aligned_begin = (raw_begin + (HEAP_ALIGNMENT - 1u)) &
                              ~(uintptr_t)(HEAP_ALIGNMENT - 1u);
    size_t prefix = (size_t)(aligned_begin - raw_begin);
    if (prefix >= size) return;

    size_t usable = size - prefix;
    usable &= ~(size_t)(HEAP_ALIGNMENT - 1u);
    if (usable < sizeof(HeapNode) + HEAP_ALIGNMENT) return;
    if (aligned_begin > UINTPTR_MAX - usable) return;

    head = (HeapNode *)aligned_begin;
    head->size = usable - sizeof(HeapNode);
    head->next = NULL;
    head->free = 1;
    head->reserved = 0;

    heap_begin = aligned_begin;
    heap_end = aligned_begin + usable;
}

void *kmalloc(size_t size) {
    if (!head || size == 0 || size > SIZE_MAX - (HEAP_ALIGNMENT - 1u))
        return NULL;

    size = HEAP_ALIGN_UP(size);
    for (HeapNode *current = head; current; current = current->next) {
        if (!current->free || current->size < size) continue;

        size_t remaining = current->size - size;
        if (remaining >= sizeof(HeapNode) + HEAP_ALIGNMENT) {
            uintptr_t next_addr = (uintptr_t)(current + 1) + size;
            if (next_addr > heap_end ||
                heap_end - next_addr < sizeof(HeapNode)) {
                continue;
            }

            HeapNode *next = (HeapNode *)next_addr;
            next->size = remaining - sizeof(HeapNode);
            next->next = current->next;
            next->free = 1;
            next->reserved = 0;

            current->size = size;
            current->next = next;
        }

        current->free = 0;
        return (void *)(current + 1);
    }
    return NULL;
}

void kfree(void *ptr) {
    if (!ptr || !head) return;

    // Only accept an exact payload pointer belonging to a live allocation.
    HeapNode *node = NULL;
    for (HeapNode *current = head; current; current = current->next) {
        if ((void *)(current + 1) == ptr) {
            node = current;
            break;
        }
    }
    if (!node || node->free) return;
    node->free = 1;

    // Coalesce adjacent free blocks.
    for (HeapNode *current = head; current && current->next;) {
        if (current->free && current->next->free) {
            current->size += sizeof(HeapNode) + current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}
