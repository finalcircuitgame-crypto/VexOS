#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "ctype.h"
#include <stdint.h>
#include <stdarg.h>

// Syscall wrappers
static inline uint64_t syscall0(uint64_t n) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n) : "memory", "rcx", "r11");
    return ret;
}

static inline uint64_t syscall1(uint64_t n, uint64_t a1) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1) : "memory", "rcx", "r11");
    return ret;
}

static inline uint64_t syscall2(uint64_t n, uint64_t a1, uint64_t a2) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2) : "memory", "rcx", "r11");
    return ret;
}

static inline uint64_t syscall3(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "memory", "rcx", "r11");
    return ret;
}

void exit(int status) {
    syscall0(0);
    while(1);
}

// stdio.h
static FILE _files[16];
static int _next_file = 0;

FILE* stdin = &_files[0];
FILE* stdout = &_files[1];
FILE* stderr = &_files[2];

void __attribute__((constructor)) init_stdio() {
    _files[0].fd = 0;
    _files[1].fd = 1;
    _files[2].fd = 2;
    _next_file = 3;
}

FILE* fopen(const char* filename, const char* mode) {
    int fd = (int)syscall1(3, (uint64_t)filename);
    if (fd < 0) return NULL;
    FILE* f = &_files[_next_file++];
    f->fd = fd;
    return f;
}

int fclose(FILE* stream) {
    if (!stream) return -1;
    syscall1(5, (uint64_t)stream->fd);
    return 0;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!stream) return 0;
    int res = (int)syscall3(4, (uint64_t)stream->fd, (uint64_t)ptr, (uint64_t)(size * nmemb));
    if (res < 0) return 0;
    return (size_t)res / size;
}

int fseek(FILE* stream, long int offset, int whence) {
    if (!stream) return -1;
    int res = (int)syscall3(6, (uint64_t)stream->fd, (uint64_t)offset, (uint64_t)whence);
    return (res >= 0) ? 0 : -1;
}

long int ftell(FILE* stream) {
    if (!stream) return -1;
    // whence 1 = SEEK_CUR with offset 0 returns current pos
    return (long int)syscall3(6, (uint64_t)stream->fd, 0, 1);
}

int puts(const char* s) {
    syscall2(1, (uint64_t)s, (uint64_t)strlen(s));
    syscall2(1, (uint64_t)"\n", 1);
    return 0;
}

int putchar(int c) {
    char ch = (char)c;
    syscall2(1, (uint64_t)&ch, 1);
    return c;
}

// Very basic printf implementations for Doom
static void print_num(int num) {
    if (num == 0) {
        putchar('0');
        return;
    }
    if (num < 0) {
        putchar('-');
        num = -num;
    }
    char buf[32];
    int i = 0;
    while (num > 0) {
        buf[i++] = (num % 10) + '0';
        num /= 10;
    }
    while (i > 0) putchar(buf[--i]);
}

int printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    for(const char* p = format; *p; p++) {
        if (*p == '%') {
            p++;
            if (*p == 'd' || *p == 'i') {
                print_num(va_arg(args, int));
            } else if (*p == 's') {
                const char* s = va_arg(args, const char*);
                if (s) syscall2(1, (uint64_t)s, strlen(s));
            } else if (*p == 'c') {
                putchar(va_arg(args, int));
            } else {
                putchar('%');
                putchar(*p);
            }
        } else {
            putchar(*p);
        }
    }
    va_end(args);
    return 0;
}

int fprintf(FILE* stream, const char* format, ...) {
    // For now, redirect all fprintf to the console output just like printf
    va_list args;
    va_start(args, format);
    for(const char* p = format; *p; p++) {
        if (*p == '%') {
            p++;
            if (*p == 'd' || *p == 'i') {
                print_num(va_arg(args, int));
            } else if (*p == 's') {
                const char* s = va_arg(args, const char*);
                if (s) syscall2(1, (uint64_t)s, strlen(s));
            } else if (*p == 'c') {
                putchar(va_arg(args, int));
            } else {
                putchar('%');
                putchar(*p);
            }
        } else {
            putchar(*p);
        }
    }
    va_end(args);
    return 0;
}

int sprintf(char* str, const char* format, ...) {
    // Dummy, not properly formatting to string
    str[0] = 0;
    return 0;
}

int snprintf(char* str, size_t size, const char* format, ...) {
    if (size > 0) str[0] = 0;
    return 0;
}

int vsnprintf(char* str, size_t size, const char* format, va_list ap) {
    if (size > 0) str[0] = 0;
    return 0;
}

// stdlib.h (bump allocator)
static uint8_t* heap_ptr = NULL;
static size_t heap_used = 0;
static size_t heap_cap = 0;

void* malloc(size_t size) {
    // align to 8
    size = (size + 7) & ~7;
    if (heap_used + size > heap_cap) {
        size_t alloc_size = size > (1024*1024) ? size : (1024*1024); // 1MB chunks
        void* new_mem = (void*)syscall2(2, 0, alloc_size);
        if ((uint64_t)new_mem == -1ULL) return NULL;
        if (!heap_ptr) {
            heap_ptr = (uint8_t*)new_mem;
            heap_cap = alloc_size;
            heap_used = 0;
        } else {
            // Cannot easily grow contiguous bump allocator if separated
            // Just use the new block if it's large enough for this alloc
            // (Memory leaks old block, but fine for doom which allocates mostly at start)
            heap_ptr = (uint8_t*)new_mem;
            heap_cap = alloc_size;
            heap_used = 0;
        }
    }
    void* ret = heap_ptr + heap_used;
    heap_used += size;
    return ret;
}

void free(void* ptr) {
    // Dummy free
}

void* calloc(size_t num, size_t size) {
    void* p = malloc(num * size);
    if (p) memset(p, 0, num * size);
    return p;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    void* new_ptr = malloc(size);
    if (new_ptr) memcpy(new_ptr, ptr, size); // Might read out of bounds of old alloc
    free(ptr);
    return new_ptr;
}

int atoi(const char* str) {
    int res = 0;
    for (int i = 0; str[i] != '\0'; ++i) {
        if (str[i] >= '0' && str[i] <= '9') {
            res = res * 10 + str[i] - '0';
        }
    }
    return res;
}

// string.h
void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = dest; const uint8_t* s = src;
    while(n--) *d++ = *s++;
    return dest;
}

void* memset(void* s, int c, size_t n) {
    uint8_t* p = s;
    while(n--) *p++ = (uint8_t)c;
    return s;
}

void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = dest; const uint8_t* s = src;
    if (d < s) {
        while(n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while(n--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* p1 = s1; const uint8_t* p2 = s2;
    while(n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t len = 0;
    while(*s++) len++;
    return len;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while((*d++ = *src++));
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    char* d = dest;
    while(n && (*d++ = *src++)) n--;
    while(n--) *d++ = 0;
    return dest;
}

int strcmp(const char* s1, const char* s2) {
    while(*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while(n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

char* strchr(const char* s, int c) {
    while(*s) { if (*s == c) return (char*)s; s++; }
    if (c == 0) return (char*)s;
    return NULL;
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    while(*s) { if (*s == c) last = s; s++; }
    if (c == 0) return (char*)s;
    return (char*)last;
}

char* strcat(char* dest, const char* src) {
    strcpy(dest + strlen(dest), src);
    return dest;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char* h = haystack;
            const char* n = needle;
            while (*h && *n && *h == *n) { h++; n++; }
            if (!*n) return (char*)haystack;
        }
    }
    return NULL;
}

// ctype.h
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
