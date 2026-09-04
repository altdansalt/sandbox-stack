/* memory.grow probe: grow within max must succeed, beyond max must fail (-1), never realloc. */
#include "../libc/libc.h"
static void put(const char* s) { write(1, s, strlen(s)); }
int main(void) {
    unsigned long pages = __builtin_wasm_memory_size(0);
    put(pages == 16 ? "initial pages: 16\n" : "initial pages: unexpected\n");
    put(__builtin_wasm_memory_grow(0, 100) == 16 ? "grow 100: ok (old=16)\n" : "grow 100: FAIL\n");
    put(__builtin_wasm_memory_size(0) == 116 ? "size now 116\n" : "size FAIL\n");
    put(__builtin_wasm_memory_grow(0, 200) == (unsigned long)-1 ? "grow 200 past max: refused\n" : "grow past max: ACCEPTED (BAD)\n");
    put(__builtin_wasm_memory_size(0) == 116 ? "size still 116\n" : "size FAIL\n");
    /* touch the newly grown region: must be zero and writable */
    volatile unsigned char* p = (unsigned char*)(115u * 65536u + 100);
    put(*p == 0 ? "new page is zero\n" : "new page NOT zero (BAD)\n");
    *p = 7;
    put(*p == 7 ? "new page writable\n" : "write FAIL\n");
    /* one byte past current size must trap */
    volatile unsigned char* q = (unsigned char*)(116u * 65536u);
    put("reading one byte past end (must trap)...\n");
    (void)*q;
    put("survived (BAD)\n");
    return 0;
}
