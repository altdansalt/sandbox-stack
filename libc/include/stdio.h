#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>
/* FILE is either a std stream (fd 0/1/2) or a memory file: input files are
   stdin slurped whole; output files are buffered and emitted to stdout on
   fclose as "@@FILE <name> <length>\n" followed by the bytes. */
typedef struct FILE { int fd; char* name; char* buf; size_t len, cap, pos; int out; } FILE;
extern FILE* stdin; extern FILE* stdout; extern FILE* stderr;
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define EOF (-1)
FILE* fopen(const char* path, const char* mode);
int fclose(FILE* f);
size_t fread(void* p, size_t sz, size_t n, FILE* f);
size_t fwrite(const void* p, size_t sz, size_t n, FILE* f);
int fseek(FILE* f, long off, int whence);
long ftell(FILE* f);
void rewind(FILE* f);
int fputc(int c, FILE* f);
int fputs(const char* s, FILE* f);
int fprintf(FILE* f, const char* fmt, ...);
int sprintf(char* buf, const char* fmt, ...);
int vfprintf(FILE* f, const char* fmt, va_list ap);
int printf(const char* fmt, ...);
int remove(const char* path);
#endif
