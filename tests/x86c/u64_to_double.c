#include "prelude.h"
double conv(unsigned long x) { return x; }
int main(void) {
  if (conv(5) != 5.0) return 1;
  if (conv(18446744073709551615UL) != 18446744073709551616.0) return 2;   /* rounds up to 2^64 */
  if (conv(9223372036854775808UL) != 9223372036854775808.0) return 3;
  return 0;
}
