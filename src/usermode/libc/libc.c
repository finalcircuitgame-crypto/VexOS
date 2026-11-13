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
static int _stdio_initialized;

FILE* stdin = &_files[0];
FILE* stdout = &_files[1];
FILE* stderr = &_files[2];

static void ensure_stdio(void) {
    if (_stdio_initialized) return;
    _files[0].fd = 0;
    _files[1].fd = 1;
    _files[2].fd = 2;
    for (int i = 3; i < 16; i++) _files[i].fd = -1;
    _stdio_initialized = 1;
}

void __attribute__((constructor)) init_stdio(void) {
    ensure_stdio();
}

FILE* fopen(const char* filename, const char* mode) {
    if (!filename || !mode || mode[0] != 'r' || mode[1] == '+')
        return NULL;
    ensure_stdio();

    int slot = -1;
    for (int i = 3; i < 16; i++) {
        if (_files[i].fd == -1) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return NULL;

    int fd = (int)syscall1(3, (uint64_t)filename);
    if (fd < 0) return NULL;
    _files[slot].fd = fd;
    return &_files[slot];
}

int fclose(FILE* stream) {
    ensure_stdio();
    if (!stream || stream < &_files[0] || stream >= &_files[16])
        return -1;
    if (stream->fd < 3) return -1;
    if (syscall1(5, (uint64_t)stream->fd) == (uint64_t)-1)
        return -1;
    stream->fd = -1;
    return 0;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    ensure_stdio();
    if (!stream || stream < &_files[0] || stream >= &_files[16] ||
        stream->fd < 0 || !ptr || size == 0 || nmemb == 0)
        return 0;
    if (nmemb > SIZE_MAX / size) return 0;

    size_t request = size * nmemb;
    if (request > UINT32_MAX) request = UINT32_MAX;
    int result = (int)syscall3(4, (uint64_t)stream->fd,
                              (uint64_t)ptr, (uint64_t)request);
    if (result <= 0) return 0;
    return (size_t)result / size;
}

int fseek(FILE* stream, long int offset, int whence) {
    ensure_stdio();
    if (!stream || stream < &_files[0] || stream >= &_files[16] ||
        stream->fd < 0 || offset < INT32_MIN || offset > INT32_MAX)
        return -1;
    int result = (int)syscall3(6, (uint64_t)stream->fd,
                               (uint64_t)(int64_t)offset, (uint64_t)whence);
    return result >= 0 ? 0 : -1;
}

long int ftell(FILE* stream) {
    ensure_stdio();
    if (!stream || stream < &_files[0] || stream >= &_files[16] ||
        stream->fd < 0)
        return -1;
    return (long int)syscall3(6, (uint64_t)stream->fd, 0, 1);
}

int puts(const char* s) {
    if (!s) return -1;
    syscall2(1, (uint64_t)s, (uint64_t)strlen(s));
    syscall2(1, (uint64_t)"\n", 1);
    return 0;
}

int putchar(int c) {
    char ch = (char)c;
    syscall2(1, (uint64_t)&ch, 1);
    return (unsigned char)ch;
}

/* Small freestanding printf-family formatter for the user-mode runtime. */
static size_t format_append(char *out, size_t capacity, size_t count, char ch) {
    if (capacity > 0 && count < capacity - 1) out[count] = ch;
    return count + 1;
}

static size_t format_unsigned(char *out, size_t capacity, size_t count,
                              uint64_t value, unsigned base, int uppercase,
                              int width, int precision, int left, int zero,
                              int alternate, char sign, int pointer_value) {
    char digits[32];
    size_t digit_count = 0;
    const char *alphabet = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (value == 0) {
        if (precision != 0) digits[digit_count++] = '0';
    } else {
        while (value && digit_count < sizeof(digits)) {
            digits[digit_count++] = alphabet[value % base];
            value /= base;
        }
    }

    size_t prefix_count = 0;
    char prefix[2];
    if (pointer_value || (alternate && base == 16 && digit_count > 0)) {
        prefix[0] = '0';
        prefix[1] = uppercase ? 'X' : 'x';
        prefix_count = 2;
    } else if (alternate && base == 8 &&
               (digit_count == 0 || digits[digit_count - 1] != '0')) {
        prefix[0] = '0';
        prefix_count = 1;
    }

    size_t zeros = precision > (int)digit_count
                       ? (size_t)precision - digit_count
                       : 0;
    size_t body = digit_count + zeros + prefix_count + (sign ? 1u : 0u);
    size_t padding = width > (int)body ? (size_t)width - body : 0;

    if (!left && !(zero && precision < 0)) {
        while (padding--) count = format_append(out, capacity, count, ' ');
        padding = width > (int)body ? (size_t)width - body : 0;
    }
    if (sign) count = format_append(out, capacity, count, sign);
    for (size_t i = 0; i < prefix_count; i++)
        count = format_append(out, capacity, count, prefix[i]);

    if (!left && zero && precision < 0) {
        while (padding--) count = format_append(out, capacity, count, '0');
    }
    while (zeros--) count = format_append(out, capacity, count, '0');
    while (digit_count > 0)
        count = format_append(out, capacity, count, digits[--digit_count]);
    if (left) {
        while (padding--) count = format_append(out, capacity, count, ' ');
    }
    return count;
}

static uint64_t format_unsigned_arg(va_list *args, int length) {
    switch (length) {
    case 1: return (unsigned char)va_arg(*args, unsigned int); /* hh */
    case 2: return (unsigned short)va_arg(*args, unsigned int); /* h */
    case 3: return va_arg(*args, unsigned long); /* l */
    case 4: return va_arg(*args, unsigned long long); /* ll */
    case 5: return va_arg(*args, size_t); /* z */
    case 6: return (uint64_t)va_arg(*args, ptrdiff_t); /* t */
    case 7: return va_arg(*args, uint64_t); /* j */
    default: return va_arg(*args, unsigned int);
    }
}

static int64_t format_signed_arg(va_list *args, int length) {
    switch (length) {
    case 1: return (signed char)va_arg(*args, int);
    case 2: return (short)va_arg(*args, int);
    case 3: return va_arg(*args, long);
    case 4: return va_arg(*args, long long);
    case 5: return (int64_t)va_arg(*args, ptrdiff_t);
    case 6: return va_arg(*args, ptrdiff_t);
    case 7: return va_arg(*args, int64_t);
    default: return va_arg(*args, int);
    }
}

int vsnprintf(char* str, size_t size, const char* format, va_list args) {
    if (!format || (!str && size != 0)) return -1;

    size_t count = 0;
    for (const char *p = format; *p; p++) {
        if (*p != '%') {
            count = format_append(str, size, count, *p);
            continue;
        }
        p++;
        if (*p == '%') {
            count = format_append(str, size, count, '%');
            continue;
        }
        if (!*p) break;

        int left = 0, plus = 0, space = 0, zero = 0, alternate = 0;
        int flags_done = 0;
        while (!flags_done) {
            switch (*p) {
            case '-': left = 1; p++; break;
            case '+': plus = 1; p++; break;
            case ' ': space = 1; p++; break;
            case '0': zero = 1; p++; break;
            case '#': alternate = 1; p++; break;
            default: flags_done = 1; break;
            }
        }

        int width = 0;
        if (*p == '*') {
            width = va_arg(args, int);
            p++;
            if (width < 0) { left = 1; width = -width; }
        } else {
            while (*p >= '0' && *p <= '9') {
                if (width < 100000) width = width * 10 + (*p - '0');
                p++;
            }
        }

        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            if (*p == '*') {
                precision = va_arg(args, int);
                p++;
                if (precision < 0) precision = -1;
            } else {
                while (*p >= '0' && *p <= '9') {
                    if (precision < 100000) precision = precision * 10 + (*p - '0');
                    p++;
                }
            }
        }

        int length = 0;
        if (*p == 'h') { p++; length = 2; if (*p == 'h') { p++; length = 1; } }
        else if (*p == 'l') { p++; length = 3; if (*p == 'l') { p++; length = 4; } }
        else if (*p == 'z') { p++; length = 5; }
        else if (*p == 't') { p++; length = 6; }
        else if (*p == 'j') { p++; length = 7; }

        switch (*p) {
        case 's': {
            const char *s = va_arg(args, const char *);
            if (!s) s = "(null)";
            size_t n = 0;
            while (s[n] && (precision < 0 || n < (size_t)precision)) n++;
            size_t pad = width > (int)n ? (size_t)width - n : 0;
            if (!left) while (pad--) count = format_append(str, size, count, ' ');
            for (size_t i = 0; i < n; i++) count = format_append(str, size, count, s[i]);
            if (left) { pad = width > (int)n ? (size_t)width - n : 0; while (pad--) count = format_append(str, size, count, ' '); }
            break;
        }
        case 'c': {
            char ch = (char)va_arg(args, int);
            size_t pad = width > 1 ? (size_t)width - 1 : 0;
            if (!left) while (pad--) count = format_append(str, size, count, ' ');
            count = format_append(str, size, count, ch);
            if (left) { pad = width > 1 ? (size_t)width - 1 : 0; while (pad--) count = format_append(str, size, count, ' '); }
            break;
        }
        case 'd':
        case 'i': {
            int64_t signed_value = format_signed_arg(&args, length);
            char sign = signed_value < 0 ? '-' : (plus ? '+' : (space ? ' ' : 0));
            uint64_t magnitude = signed_value < 0 ? (uint64_t)(0ULL - (uint64_t)signed_value)
                                                  : (uint64_t)signed_value;
            count = format_unsigned(str, size, count, magnitude, 10, 0,
                                     width, precision, left, zero, 0, sign, 0);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o':
        case 'p': {
            uint64_t value = *p == 'p'
                                 ? (uint64_t)(uintptr_t)va_arg(args, void *)
                                 : format_unsigned_arg(&args, length);
            unsigned base = (*p == 'x' || *p == 'X' || *p == 'p') ? 16u :
                            (*p == 'o' ? 8u : 10u);
            count = format_unsigned(str, size, count, value, base,
                                     *p == 'X', width, precision, left,
                                     zero, alternate, 0, *p == 'p');
            break;
        }
        default:
            count = format_append(str, size, count, '%');
            count = format_append(str, size, count, *p);
            break;
        }
    }

    if (size > 0) str[count < size ? count : size - 1] = '\0';
    return count > INT32_MAX ? INT32_MAX : (int)count;
}

int snprintf(char* str, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsnprintf(str, size, format, args);
    va_end(args);
    return result;
}

int sprintf(char* str, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsnprintf(str, SIZE_MAX, format, args);
    va_end(args);
    return result;
}

int printf(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    int needed = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (needed < 0) return needed;
    size_t written = (size_t)needed < sizeof(buffer) ? (size_t)needed : sizeof(buffer) - 1;
    if (written) syscall2(1, (uint64_t)buffer, written);
    return needed;
}

int fprintf(FILE* stream, const char* format, ...) {
    if (!stream) return -1;
    char buffer[1024];
    va_list args;
    va_start(args, format);
    int needed = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (needed < 0) return needed;
    size_t written = (size_t)needed < sizeof(buffer) ? (size_t)needed : sizeof(buffer) - 1;
    if (written) syscall2(1, (uint64_t)buffer, written);
    return needed;
}

// stdlib.h: simple page-backed bump allocator. free() remains a no-op, but
// allocation sizes are recorded so realloc never reads beyond the old block.
typedef struct __attribute__((aligned(16))) {
    size_t size;
    uint64_t reserved;
} UserAllocHeader;

static uint8_t* heap_ptr = NULL;
static size_t heap_used = 0;
static size_t heap_cap = 0;

static int heap_grow(size_t required) {
    const size_t page_size = 4096u;
    const size_t max_map = 16u * 1024u * 1024u;
    size_t amount = required > (1024u * 1024u) ? required : (1024u * 1024u);
    if (amount > max_map) amount = max_map;
    if (amount > SIZE_MAX - (page_size - 1u)) return 0;
    amount = (amount + page_size - 1u) & ~(page_size - 1u);
    if (amount < required) return 0;

    uint64_t requested = heap_ptr ? (uint64_t)(uintptr_t)(heap_ptr + heap_cap) : 0;
    void *mapped = (void *)syscall2(2, requested, amount);
    if ((uint64_t)(uintptr_t)mapped == UINT64_MAX || !mapped) return 0;

    if (!heap_ptr) {
        heap_ptr = (uint8_t *)mapped;
        heap_cap = amount;
        heap_used = 0;
    } else {
        if (mapped != heap_ptr + heap_cap) return 0;
        if (heap_cap > SIZE_MAX - amount) return 0;
        heap_cap += amount;
    }
    return 1;
}

void* malloc(size_t size) {
    if (size == 0) size = 1;
    if (size > SIZE_MAX - sizeof(UserAllocHeader) - 15u) return NULL;

    size_t aligned = (size + 15u) & ~(size_t)15u;
    size_t total = sizeof(UserAllocHeader) + aligned;
    if (heap_used > heap_cap || total > heap_cap - heap_used) {
        size_t missing = total;
        if (heap_cap == 0 || heap_used == 0) {
            if (!heap_grow(missing)) return NULL;
        } else {
            if (!heap_grow(missing)) return NULL;
        }
    }

    UserAllocHeader *header = (UserAllocHeader *)(heap_ptr + heap_used);
    header->size = size;
    header->reserved = 0;
    void *result = (void *)(header + 1);
    heap_used += total;
    return result;
}

void free(void* ptr) {
    // The current allocator is intentionally monotonic; released blocks are
    // retained until process exit. Do not dereference arbitrary pointers here.
    (void)ptr;
}

void* calloc(size_t num, size_t size) {
    if (size && num > SIZE_MAX / size) return NULL;
    size_t total = num * size;
    void* p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    UserAllocHeader *old_header = ((UserAllocHeader *)ptr) - 1;
    size_t old_size = old_header->size;
    if (size <= old_size) {
        old_header->size = size;
        return ptr;
    }

    void* new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_size);
    free(ptr);
    return new_ptr;
}

int atoi(const char* str) {
    int res = 0;
    int sign = 1;
    if (!str) return 0;
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') str++;
    if (*str == '-' || *str == '+') {
        if (*str++ == '-') sign = -1;
    }
    while (*str >= '0' && *str <= '9') {
        if (res > (INT32_MAX - 9) / 10) break;
        res = res * 10 + (*str++ - '0');
    }
    return sign * res;
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
