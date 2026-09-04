#include "../libc/libc.h"
int main(void) {
    static char buf[4096];
    for (;;) {
        ssize_t n = read(0, buf, sizeof buf);
        if (n <= 0) return n < 0 ? 1 : 0;
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(1, buf + off, (size_t)(n - off));
            if (w <= 0) return 1;
            off += w;
        }
    }
}
