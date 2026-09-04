/* Deliberately misbehaving guest: writes a marker, then tries to make the host
   call something that is not read/write/exit. It has no direct syscall access,
   so the only lever is the imports; we test the host by asking for fd 3 and
   also by out-of-bounds memory access (must trap, not escape). */
#include "libc.h"
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    write(1, "evil: about to read out of bounds\n", 34);
    volatile int* p = (volatile int*)0xFFFFFFF0u;
    int v = *p;                 /* out of bounds: must trap */
    write(1, "evil: survived OOB (BAD)\n", 25);
    return v;
}
