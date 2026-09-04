#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
void* malloc(size_t n);
void* calloc(size_t n, size_t m);
void* realloc(void* p, size_t n);
void free(void* p);
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
void exit(int code) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
unsigned long strtoul(const char* s, char** end, int base);
void qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*));
#endif
