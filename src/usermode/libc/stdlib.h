#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t num, size_t size);
void* realloc(void* ptr, size_t size);

void exit(int status);
int atoi(const char* str);
double atof(const char* str);
int abs(int value);
int remove(const char* path);
int rename(const char* oldpath, const char* newpath);
int system(const char* command);
int mkdir(const char* path, unsigned int mode);

#endif
