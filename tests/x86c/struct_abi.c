#include "prelude.h"
#include <stdarg.h>
struct S { long a, b, c; };           /* 24 bytes: hidden return pointer */
struct T { int a; char b; };          /* 8 bytes: returned in a register */
struct S mk(long x) { struct S s; s.a = x; s.b = x * 2; s.c = x * 3; return s; }
struct T mt(int x) { struct T t; t.a = x; t.b = (char)(x + 1); return t; }
long sum(struct S s) { return s.a + s.b + s.c; }
int va(int n, ...) { va_list ap; int i, s = 0; va_start(ap, n); for (i = 0; i < n; i++) s += va_arg(ap, int); va_end(ap); return s; }
int main(void) { struct S s = mk(7); struct T t = mt(3); if (sum(s) != 42) return 1; if (t.a != 3 || t.b != 4) return 2; if (va(3, 1, 2, 3) != 6) return 3; return 0; }
