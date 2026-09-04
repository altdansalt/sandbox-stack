#include "prelude.h"
void fill(char *p, unsigned long n) { unsigned long i; for (i = 0; i < n; i++) p[i] = (char)i; }
int main(void) { unsigned long n = 40; char *p = alloca(n); fill(p, n); return (p[0] == 0 && p[39] == 39) ? 0 : 1; }
