#include "libc.h"
static int g(int x) { int r = 0; switch (x) { case 1: r += 1; case 2: r += 2; break; case 3 ... 5: r = 30; break; default: r = -1; } return r; }
static int h(void) { int i, n = 0; for (i = 0; i < 10; i++) { switch (i) { case 3: continue; case 7: break; default: n++; } if (i == 8) break; } return n; }
int main(int argc, char** argv) { (void)argc; (void)argv; return (g(1) == 3 && g(2) == 2 && g(4) == 30 && g(9) == -1 && h() == 7) ? 0 : 1; }
