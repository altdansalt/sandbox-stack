/* Asks the host for fd 3. A correct host refuses (returns -1). With the host
   built -DDEMO_ESCAPE, fd 3 makes the host issue getpid(2): the kernel must SIGKILL. */
#include "../libc/libc.h"
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    write(1, "escape: asking host for fd 3\n", 29);
    int r = write(3, "x", 1);
    write(1, r < 0 ? "escape: host refused fd 3 (good)\n" : "escape: fd 3 worked (BAD)\n", r < 0 ? 33 : 26);
    return 0;
}
