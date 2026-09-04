/* libc extension needed to run w2c2 as a guest: memory FILEs, integer printf,
   qsort, strtoul, misc string functions, exit/abort/assert. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int errno;
static FILE std_files[3] = { {0}, {1}, {2} };
FILE* stdin = &std_files[0];
FILE* stdout = &std_files[1];
FILE* stderr = &std_files[2];

static void write_all(int fd, const char* p, size_t n) {
    while (n) { ssize_t w = write(fd, p, n); if (w <= 0) _exit(120); p += w; n -= (size_t)w; }
}

FILE* fopen(const char* path, const char* mode) {
    FILE* f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->fd = -1;
    f->name = malloc(strlen(path) + 1);
    strcpy(f->name, path);
    if (mode[0] == 'r') {            /* the single input: slurp stdin */
        f->cap = 65536; f->buf = malloc(f->cap);
        for (;;) {
            ssize_t n;
            if (f->len == f->cap) { f->cap *= 2; f->buf = realloc(f->buf, f->cap); }
            n = read(0, f->buf + f->len, f->cap - f->len);
            if (n < 0) return NULL;
            if (n == 0) break;
            f->len += (size_t)n;
        }
    } else {
        f->out = 1; f->cap = 65536; f->buf = malloc(f->cap);
    }
    return f;
}
size_t fwrite(const void* p, size_t sz, size_t n, FILE* f) {
    size_t total = sz * n;
    if (f->fd >= 0) { write_all(f->fd, p, total); return n; }
    if (!f->out) return 0;
    while (f->len + total > f->cap) { f->cap *= 2; f->buf = realloc(f->buf, f->cap); }
    memcpy(f->buf + f->len, p, total);
    f->len += total;
    return n;
}
size_t fread(void* p, size_t sz, size_t n, FILE* f) {
    size_t total = sz * n;
    if (f->fd >= 0 || f->out) return 0;
    if (total > f->len - f->pos) total = f->len - f->pos;
    memcpy(p, f->buf + f->pos, total);
    f->pos += total;
    return total / sz;
}
int fseek(FILE* f, long off, int whence) {
    size_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? f->pos : f->len;
    f->pos = base + (size_t)off;
    return 0;
}
long ftell(FILE* f) { return (long)f->pos; }
void rewind(FILE* f) { f->pos = 0; }
int fclose(FILE* f) {
    if (f->fd >= 0) return 0;
    if (f->out) {
        char hdr[64];
        sprintf(hdr, "@@FILE %s %lu\n", f->name, (unsigned long)f->len);
        write_all(1, hdr, strlen(hdr));
        write_all(1, f->buf, f->len);
    }
    return 0;
}
int fputc(int c, FILE* f) { char ch = (char)c; fwrite(&ch, 1, 1, f); return c; }
int fputs(const char* s, FILE* f) { fwrite(s, 1, strlen(s), f); return 0; }
int remove(const char* path) { (void)path; return 0; }
int chdir(const char* path) { (void)path; return 0; }

/* integer-only printf core: flags '0' '-', width, precision (strings), l/ll/z, d i u x X c s % */
typedef struct { FILE* f; char* s; } Sink;
static void put(Sink* k, const char* p, size_t n) {
    if (k->f) fwrite(p, 1, n, k->f); else { memcpy(k->s, p, n); k->s += n; }
}
static int format(Sink* k, const char* fmt, va_list ap) {
    int count = 0;
    while (*fmt) {
        const char* start = fmt;
        char num[24]; int zero = 0, left = 0, width = 0, prec = -1, longs = 0, neg = 0, len;
        const char *digits, *body; unsigned long long v;
        if (*fmt != '%') { while (*fmt && *fmt != '%') fmt++; put(k, start, (size_t)(fmt - start)); count += (int)(fmt - start); continue; }
        fmt++;
        for (;; fmt++) { if (*fmt == '0') zero = 1; else if (*fmt == '-') left = 1; else break; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') { fmt++; prec = 0; while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
        while (*fmt == 'l' || *fmt == 'z') { longs++; fmt++; }
        digits = "0123456789abcdef";
        switch (*fmt++) {
        case 's': body = va_arg(ap, const char*); if (!body) body = "(null)"; len = (int)strlen(body); if (prec >= 0 && len > prec) len = prec; break;
        case 'c': num[0] = (char)va_arg(ap, int); body = num; len = 1; break;
        case '%': body = "%"; len = 1; break;
        case 'd': case 'i': {
            long long sv = longs >= 2 ? va_arg(ap, long long) : (long long)va_arg(ap, int);
            if (sv < 0) { neg = 1; v = (unsigned long long)(-(sv + 1)) + 1; } else v = (unsigned long long)sv;
            goto number;
        }
        case 'u': v = longs >= 2 ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned); goto number;
        case 'X': digits = "0123456789ABCDEF"; /* fallthrough */
        case 'x': v = longs >= 2 ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned);
            len = 0; do { num[23 - len++] = digits[v & 15]; v >>= 4; } while (v); body = num + 24 - len; break;
        number:
            len = 0; do { num[23 - len++] = digits[v % 10]; v /= 10; } while (v);
            if (neg) num[23 - len++] = '-';
            body = num + 24 - len; break;
        default: abort(); /* floats and anything else are unsupported on purpose */
        }
        if (!left) while (width > len) { put(k, zero ? "0" : " ", 1); width--; count++; }
        put(k, body, (size_t)len); count += len;
        if (left) while (width > len) { put(k, " ", 1); width--; count++; }
    }
    return count;
}
int vfprintf(FILE* f, const char* fmt, va_list ap) { Sink k = { f, 0 }; return format(&k, fmt, ap); }
int fprintf(FILE* f, const char* fmt, ...) { va_list ap; int r; va_start(ap, fmt); r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int printf(const char* fmt, ...) { va_list ap; int r; va_start(ap, fmt); r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }
int sprintf(char* buf, const char* fmt, ...) {
    va_list ap; int r; Sink k = { 0, buf };
    va_start(ap, fmt); r = format(&k, fmt, ap); va_end(ap); *k.s = 0; return r;
}

void exit(int code) { _exit(code); }
void abort(void) { write_all(2, "abort\n", 6); _exit(134); }
void __assert_fail(const char* e, const char* file, int line) {
    fprintf(stderr, "assertion failed: %s (%s:%d)\n", e, file, line); abort();
}
char* strerror(int e) { (void)e; return "error"; }
int strncmp(const char* a, const char* b, size_t n) {
    for (; n; n--, a++, b++) { if (*a != *b) return (unsigned char)*a - (unsigned char)*b; if (!*a) return 0; }
    return 0;
}
char* strncpy(char* d, const char* s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
char* strchr(const char* s, int c) { for (;; s++) { if (*s == (char)c) return (char*)s; if (!*s) return NULL; } }
char* strrchr(const char* s, int c) { const char* r = NULL; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char*)r; } }
unsigned long strtoul(const char* s, char** end, int base) {
    unsigned long v = 0;
    if (base == 0) base = 10;
    while (*s == ' ') s++;
    for (;; s++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'z' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'Z' ? *s - 'A' + 10 : 99;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end) *end = (char*)s;
    return v;
}
/* deterministic, stable insertion sort: same result on every platform (glibc's qsort is not stable) */
void qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*)) {
    char* b = base; char tmp[64]; size_t i, j;
    if (size > sizeof tmp) abort();
    for (i = 1; i < n; i++) {
        memcpy(tmp, b + i * size, size);
        for (j = i; j > 0 && cmp(b + (j - 1) * size, tmp) > 0; j--) memcpy(b + j * size, b + (j - 1) * size, size);
        memcpy(b + j * size, tmp, size);
    }
}
