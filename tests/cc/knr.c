#include "libc.h"
int f();
int f(int x) { return x; }
int main(int argc, char** argv) { (void)argc; (void)argv; return f(7); }
