#include "libc.h"
long double x = 1.5L;
int main(int argc, char** argv) { (void)argc; (void)argv; return x == 1.5L ? 0 : 1; }
