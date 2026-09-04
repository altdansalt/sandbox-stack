#include "prelude.h"
float lit(void) { return 1.0f; }
double dlit(void) { return 2.5; }
int main(void) { volatile float x = lit(); volatile double y = dlit(); return (x == 1.0f && y == 2.5 && (int)(x + y) == 3) ? 0 : 1; }
