#include "../libc/libc.h"
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    static char buf[4096];
    for (;;) {
        ssize_t n = read(0, buf, sizeof buf);
        if (n <= 0) return n < 0 ? 1 : 0;
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c >= 'a' && c <= 'z') buf[i] = (char)('a' + (c - 'a' + 13) % 26);
            else if (c >= 'A' && c <= 'Z') buf[i] = (char)('A' + (c - 'A' + 13) % 26);
        }
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(1, buf + off, (size_t)(n - off));
            if (w <= 0) return 1;
            off += w;
        }
    }
}
