/* Host for w2c2-translated guests. x86_64 Linux, no libc.
 * Enters SECCOMP_MODE_STRICT (read/write/exit/sigreturn only), then runs the guest.
 * Compile: tcc -nostdlib -static -Irt -DARENA_PAGES=N host/main.c guest.c */
#include "guest.h"

#ifndef ARENA_PAGES
#define ARENA_PAGES 256
#endif
#ifndef TABLE_SIZE
#define TABLE_SIZE 1024
#endif
U8 wasm_arena[(unsigned long)ARENA_PAGES * WASM_PAGE_SIZE];
const U32 wasm_arena_pages = ARENA_PAGES;
wasmFunc wasm_table_arena[TABLE_SIZE];
U32 wasm_table_types_arena[TABLE_SIZE];
const U32 wasm_table_arena_size = TABLE_SIZE;

enum { SYS_read = 0, SYS_write = 1, SYS_exit = 60, SYS_prctl = 157, PR_SET_SECCOMP = 22, SECCOMP_MODE_STRICT = 1 };

static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
/* exit, not exit_group: strict seccomp kills on exit_group, which is what libc's exit() uses */
static void NORETURN sys_exit(int code) { for (;;) sys3(SYS_exit, code, 0, 0); }

void* memcpy(void* d, const void* s, size_t n) { U8* x = d; const U8* y = s; while (n--) *x++ = *y++; return d; }
void* memmove(void* d, const void* s, size_t n) {
    U8* x = d; const U8* y = s;
    if (x < y) while (n--) *x++ = *y++; else { x += n; y += n; while (n--) *--x = *--y; }
    return d;
}
void* memset(void* d, int c, size_t n) { U8* x = d; while (n--) *x++ = (U8)c; return d; }
void* calloc(size_t n, size_t m) { (void)n; (void)m; trap(trapAllocationFailed); }

void trap(Trap t) {
    char msg[] = "trap: 0\n";
    msg[6] = (char)('0' + (int)t);
    sys3(SYS_write, 2, (long)msg, 8);
    sys_exit(100 + (int)t);
}

static wasmMemory* mem(void* i) { return ((guestInstance*)i)->m0; }
static U32 ret(long r) { return r < 0 ? (U32)-1 : (U32)r; }

U32 env__read(void* i, U32 fd, U32 ptr, U32 len) {
    if (fd != 0) return (U32)-1;
    wasm_check(mem(i), ptr, len);
    return ret(sys3(SYS_read, 0, (long)(mem(i)->data + ptr), len));
}
U32 env__write(void* i, U32 fd, U32 ptr, U32 len) {
#ifdef DEMO_ESCAPE
    if (fd == 3) return (U32)sys3(39 /* getpid */, 0, 0, 0); /* demo: any other syscall must be fatal */
#endif
    if (fd != 1 && fd != 2) return (U32)-1;
    wasm_check(mem(i), ptr, len);
    return ret(sys3(SYS_write, fd, (long)(mem(i)->data + ptr), len));
}
void env__exit(void* i, U32 code) { (void)i; sys_exit((int)code); }

static guestInstance inst;

void cmain(void) {
    if (sys3(SYS_prctl, PR_SET_SECCOMP, SECCOMP_MODE_STRICT, 0) != 0) {
        sys3(SYS_write, 2, (long)"seccomp strict failed\n", 22);
        sys_exit(99);
    }
    guestInstantiate(&inst, NULL);
    guest__start(&inst);
    sys_exit(0);
}
__asm__(".globl _start\n_start:\n xor %rbp,%rbp\n call cmain\n hlt\n");
