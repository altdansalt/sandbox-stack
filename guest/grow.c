/* memory.grow probe: grow within max must succeed, beyond max must fail (-1), never realloc.
   Layout-independent: reads the initial size instead of assuming it. */
#include "libc.h"
static void put(const char* s) { write(1, s, strlen(s)); }
int main(int argc, char** argv) {
    unsigned long p0 = __builtin_wasm_memory_size(0);
    (void)argc; (void)argv;
    put(p0 > 0 && p0 <= 256 ? "initial pages: sane\n" : "initial pages: unexpected\n");
    put(__builtin_wasm_memory_grow(0, 100) == p0 ? "grow 100: ok (returned old size)\n" : "grow 100: FAIL\n");
    put(__builtin_wasm_memory_size(0) == p0 + 100 ? "size now old+100\n" : "size FAIL\n");
    put(__builtin_wasm_memory_grow(0, 200) == (unsigned long)-1 ? "grow 200 past max: refused\n" : "grow past max: ACCEPTED (BAD)\n");
    put(__builtin_wasm_memory_size(0) == p0 + 100 ? "size still old+100\n" : "size FAIL\n");
    {
        /* touch the last new page: must be zero and writable */
        volatile unsigned char* p = (unsigned char*)((p0 + 99) * 65536u + 100);
        put(*p == 0 ? "new page is zero\n" : "new page NOT zero (BAD)\n");
        *p = 7;
        put(*p == 7 ? "new page writable\n" : "write FAIL\n");
    }
    {
        /* one byte past current size must trap */
        volatile unsigned char* q = (unsigned char*)((p0 + 100) * 65536u);
        put("reading one byte past end (must trap)...\n");
        (void)*q;
        put("survived (BAD)\n");
    }
    return 0;
}
