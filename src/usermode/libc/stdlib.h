#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t num, size_t size);
void* realloc(void* ptr, size_t size);

void exit(int status);
int atoi(const char* str);\ndouble atof(const char* str);\nint abs(int value);\nint remove(const char* path);\nint rename(const char* oldpath, const char* newpath);\nint system(const char* command);\nint mkdir(const char* path, unsigned int mode);

#endif
