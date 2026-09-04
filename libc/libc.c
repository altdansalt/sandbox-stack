#include "libc.h"

ssize_t read(int fd, void* buf, size_t n) { return sys_read(fd, buf, (int)n); }
ssize_t write(int fd, const void* buf, size_t n) { return sys_write(fd, buf, (int)n); }
void _exit(int code) { sys_exit(code); }

/* bump allocator over linear memory; grows with memory.grow; free is a no-op */
extern unsigned char __heap_base;
static size_t heap_top;

void* malloc(size_t n) {
    size_t cur, top, end;
    if (!heap_top) heap_top = (size_t)&__heap_base;
    n = (n + 15) & ~(size_t)15;
    cur = heap_top;
    top = cur + n;
    end = (size_t)__builtin_wasm_memory_size(0) << 16;
    if (top > end) {
        size_t need = (top - end + 65535) >> 16;
        if (__builtin_wasm_memory_grow(0, need) == (size_t)-1) return NULL;
    }
    heap_top = top;
    return (void*)cur;
}
void* calloc(size_t n, size_t m) {
    void* p = malloc(n * m);
    if (p) memset(p, 0, n * m);
    return p;
}
void* realloc(void* p, size_t n) {
    /* no size header: copy n bytes (over-reads old block, but stays in our heap) */
    void* q = malloc(n);
    if (q && p) memcpy(q, p, n);
    return q;
}
void free(void* p) { (void)p; }

void* memcpy(void* d, const void* s, size_t n) {
    unsigned char* dd = d; const unsigned char* ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void* memmove(void* d, const void* s, size_t n) {
    unsigned char* dd = d; const unsigned char* ss = s;
    if (dd < ss) while (n--) *dd++ = *ss++;
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}
void* memset(void* d, int c, size_t n) {
    unsigned char* dd = d;
    while (n--) *dd++ = (unsigned char)c;
    return d;
}
int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = a; const unsigned char* y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
size_t strlen(const char* s) { const char* p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
char* strcpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)); return r; }

int main(void);
__attribute__((export_name("_start"))) void _start(void) { _exit(main()); }
