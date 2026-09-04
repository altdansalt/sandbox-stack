  .text
  .globl _start
_start:
  mov $1, %r8w
  mov $1, %r15w
  mov $-1, %ax
  mov $255, %al
  mov $-128, %sil
  mov $5, %r9b
  movw $1, (%r12)
  movw $1, 0(%r13)
  movl $-7, 8(%rsp)
  movq $-7, -8(%rbp)
  movq $2147483647, (%rax)
  mov $2147483648, %rax
  mov $-2147483649, %rcx
  mov $18446744073709551615, %r11
  mov %r8b, (%r12)
  mov (%r13), %r15
  mov %r10, 16(%r12)
  mov 0(%r13), %eax
  mov %al, (%rdi)
  mov %ax, (%rdi)
  mov %eax, (%rdi)
  mov %rax, (%rdi)
  mov %dil, %al
  mov %sil, %dil
  mov %r8w, %r9w
  mov %ecx, %r10d
  sete %sil
  sete %dil
  setne %r8b
  setae %al
  seta %al
  setp %dl
  setnp %dl
  and %dl, %al
  or %dl, %al
  and $1, %al
  addl $2147483648, %eax
  addl $4294967295, %eax
  add $127, %rax
  add $128, %rax
  add $-128, %rsp
  add $-129, %rsp
  add $12, %ah
  sub %edi, %eax
  sub %rdi, %rax
  cmp $0, %eax
  cmp $0, %rax
  cmp %edi, %eax
  cmp %r9d, %r8d
  cmp %r9, %r8
  xor %edi, %eax
  xor %r13, %r14
  and %rdi, %rax
  or %edi, %eax
  imul %edi, %eax
  imul %rdi, %rax
  imul %r9d, %r8d
  idiv %edi
  idiv %rdi
  div %edi
  div %rdi
  div %r9
  mul %rdi
  neg %rax
  neg %eax
  not %rax
  cqo
  cdq
  shl %cl, %eax
  shl %cl, %rax
  shr %cl, %eax
  sar %cl, %rax
  shl $3, %rax
  shl $31, %eax
  sar $63, %rax
  shr %rdi
  inc %rax
  dec %eax
  test %rax, %rax
  test %al, %al
  movsbl %al, %eax
  movzbl %al, %eax
  movswl %ax, %eax
  movzwl %ax, %eax
  movsxd %eax, %rax
  movzx %al, %rax
  movzb %al, %rax
  movsbl (%rax), %eax
  movzbl (%rax), %eax
  movswl (%rax), %eax
  movzwl (%rax), %eax
  movsxd (%rax), %rax
  movzbl 3(%r13), %r9d
  movsbl -24(%rsp), %eax
  lea -8(%rbp), %rax
  lea 8(%rsp), %rdi
  lea 0(%r13), %r9
  lea (%r12), %rax
  lea x(%rip), %rax
  mov x@GOTPCREL(%rip), %rax
  push %rax
  push %r9
  pop %rdi
  pop %r15
  push %rbp
  mov %rsp, %rbp
  sub $16, %rsp
  mov %rsp, -16(%rbp)
  movss (%rax), %xmm0
  movsd (%rax), %xmm0
  movss %xmm0, (%rdi)
  movsd %xmm0, (%rdi)
  movss %xmm1, %xmm0
  movsd %xmm1, %xmm0
  movsd %xmm7, 8(%rbp)
  movss -4(%rsp), %xmm0
  movq %rax, %xmm0
  movq %rax, %xmm1
  movq %xmm0, %rax
  movq %xmm1, %xmm2
  cvtsi2ssl %eax, %xmm0
  cvtsi2sdl %eax, %xmm0
  cvtsi2ssq %rax, %xmm0
  cvtsi2sdq %rax, %xmm0
  cvtsi2sd %rax, %xmm0
  cvtsi2sd %rdi, %xmm0
  cvttss2sil %xmm0, %eax
  cvttsd2sil %xmm0, %eax
  cvttss2siq %xmm0, %rax
  cvttsd2siq %xmm0, %rax
  cvtss2sd %xmm0, %xmm0
  cvtsd2ss %xmm0, %xmm0
  ucomiss %xmm1, %xmm0
  ucomisd %xmm1, %xmm0
  ucomiss %xmm0, %xmm1
  addss %xmm1, %xmm0
  subss %xmm1, %xmm0
  mulss %xmm1, %xmm0
  divss %xmm1, %xmm0
  addsd %xmm1, %xmm0
  subsd %xmm1, %xmm0
  mulsd %xmm1, %xmm0
  divsd %xmm1, %xmm0
  addsd %xmm0, %xmm0
  xorps %xmm1, %xmm1
  xorpd %xmm1, %xmm1
  pxor %xmm0, %xmm0
  test %rax,%rax; js 1f; pxor %xmm0,%xmm0; cvtsi2sd %rax,%xmm0; jmp 2f; 1: mov %rax,%rdi; and $1,%eax; pxor %xmm0,%xmm0; shr %rdi; or %rax,%rdi; cvtsi2sd %rdi,%xmm0; addsd %xmm0,%xmm0; 2:
  mov $1065353216, %eax  # float 1.000000
1:
  jmp 1b
  je 3f
  jne 3f
  jbe 3f
  jl 3f
  jle 3f
  jb 3f
3:
  call f
  call *%r10
  jmp *%rax
  mov $8, %rcx
  lea -8(%rbp), %rdi
  mov $0, %al
  rep stosb
  syscall
  ret
f:
  hlt
  .data
  .align 8
x:
  .byte 1
  .byte -1
  .zero 7
  .quad x
  .quad f+3
  .quad -5
  .bss
  .align 16
y:
  .zero 100
