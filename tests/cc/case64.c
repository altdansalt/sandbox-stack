#include "libc.h"
static int f(long long x) { switch (x) { case 0x100000000LL: return 1; case 0: return 2; } return 3; }
int main(int argc, char** argv) { (void)argc; (void)argv; return (f(0x100000000LL) == 1 && f(0) == 2 && f(5) == 3) ? 0 : 1; }
