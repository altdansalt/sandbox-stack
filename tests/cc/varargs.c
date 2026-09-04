#include "libc.h"
#include <stdarg.h>
static long long sum(int n, ...) { va_list ap; long long s = 0; int i; va_start(ap, n); for (i = 0; i < n; i++) s += va_arg(ap, long long); va_end(ap); return s; }
static double avg(int n, ...) { va_list ap; double s = 0; int i; va_start(ap, n); for (i = 0; i < n; i++) s += va_arg(ap, double); va_end(ap); return s / n; }
struct P { int a; long long b; char c; };
static struct P mk(int a) { struct P p; p.a = a; p.b = (long long)a << 40; p.c = (char)a; return p; }
static long long use(struct P p) { p.a += 1; return p.b + p.a + p.c; }
int main(int argc, char** argv) { (void)argc; (void)argv;
  struct P p = mk(3);
  if (sum(3, 1LL, 2LL, 0x100000000LL) != 0x100000003LL) return 1;
  if (avg(2, 1.0, 2.0f) != 1.5) return 2;
  if (use(p) != ((3LL << 40) + 4 + 3) || p.a != 3) return 3;
  if ((unsigned char)-1 != 255 || (signed char)200 != -56 || (unsigned short)-1 != 65535) return 4;
  if (-7 / 2 != -3 || -7 % 2 != -1 || 7u / 2u != 3 || (-1 >> 1) != -1 || (0xffffffffu >> 1) != 0x7fffffffu) return 5;
  if ((long long)(unsigned)0xffffffffu != 0xffffffffLL || (long long)(int)0xffffffffu != -1LL) return 6;
  if ((int)2.9 != 2 || (int)-2.9 != -2 || (unsigned)3000000000.0 != 3000000000u) return 7;
  return 0;
}
