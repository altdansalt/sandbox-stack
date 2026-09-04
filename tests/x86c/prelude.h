/* freestanding prelude for native x86-64 tests: _start calls main and exits with its return value */
__asm__(".globl _start\n_start:\n xor %rbp, %rbp\n call main\n mov %eax, %edi\n mov $60, %eax\n syscall\n hlt\n");
