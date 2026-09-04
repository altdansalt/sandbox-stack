/* Minimal libc for wasm32 guests. Imports: env.read, env.write, env.exit. */
#ifndef MINLIBC_H
#define MINLIBC_H
typedef unsigned long size_t;
typedef long ssize_t;
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef int int32_t;
typedef long long int64_t;
#define NULL ((void*)0)

/* host imports */
__attribute__((import_module("env"), import_name("read")))  int  sys_read(int fd, void* buf, int n);
__attribute__((import_module("env"), import_name("write"))) int  sys_write(int fd, const void* buf, int n);
__attribute__((import_module("env"), import_name("exit")))  void sys_exit(int code) __attribute__((noreturn));

ssize_t read(int fd, void* buf, size_t n);
ssize_t write(int fd, const void* buf, size_t n);
void _exit(int code) __attribute__((noreturn));
void* malloc(size_t n);
void* calloc(size_t n, size_t m);
void* realloc(void* p, size_t n);
void free(void* p);
void* memcpy(void* d, const void* s, size_t n);
void* memmove(void* d, const void* s, size_t n);
void* memset(void* d, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
char* strcpy(char* d, const char* s);
#endif
