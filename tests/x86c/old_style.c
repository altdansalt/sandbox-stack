#include "prelude.h"
int f();
int f(int x) { return x + 1; }
int main(void) { return f(41) == 42 ? 0 : 1; }
