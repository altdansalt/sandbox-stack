Review request: x86-64 assembler + static ELF writer for chibicc (cc/asm.c), and the x86 target integration.

Context: NOTES.md section "Experiment 4". cc/asm.c is new (assembler for the GNU-syntax subset chibicc's codegen emits, plus an ELF64 writer). cc/codegen_x86.c is upstream chibicc's codegen.c with small changes (diff against ~/src/chibicc/codegen.c). Target selection: cc/main.c (-mx86), cc/type.c set_target_lp64, cc/parse.c (file-scope asm, struct-return and va_area conditionals), cc/preprocess.c macros. host/main.c has a __chibicc__ branch with a file-scope asm syscall stub.

Verified so far: the instruction stream is identical to GNU as on ~1M instructions of real output (tests/asm-diff.sh), all probes/adversarial modules behave identically to tcc-built hosts, strace shows only prctl/read/write/exit.

Please look for:
1. Encoding bugs in paths NOT exercised by those inputs: 16-bit operands, high-byte registers, r8-r15 in every operand position (REX.B/REX.R), rsp/r12 and rbp/r13 as base registers (SIB / disp8 rules), imm8 vs imm32 selection incl. sign edge cases (-128, 127, 128, 0x80000000), mov $imm64, mov $imm to memory with size suffixes, shifts by immediate, setcc on sil/dil, all the SSE forms (movss/movsd load/store/reg-reg, movq both directions, cvtsi2ss/sd l/q, cvttss2si/cvttsd2si l/q, cvtss2sd, cvtsd2ss, ucomiss/ucomisd, addss..divsd, xorps/xorpd/pxor), test, imul, div/idiv, cqo/cdq, numeric local labels (1f), the ';'-separated multi-instruction cast strings in codegen_x86.c.
2. Fixup/relocation math: rel32 for rip-relative operands when an immediate follows the displacement, jcc/jmp/call rel32, .quad sym+addend, symbols in .data/.bss, sign and range checks.
3. ELF correctness: header fields, program header alignment rules, file offset vs vaddr congruence, bss sizing, entry, anything the kernel loader would reject or that would silently misplace data; also the security properties of the segments (is anything writable+executable?).
4. Anything the assembler silently accepts wrongly instead of erroring (e.g. size-ambiguous instructions, unknown directives, malformed operands, duplicate labels, undefined symbols).
5. Integration: -mx86 type sizes vs chibicc's x86 codegen assumptions (long double 16 bytes but x87 unsupported, va_area, struct return), file-scope asm parsing, opt_fcommon=false consequences, and whether the wasm path could be affected by the changes.

Be concrete: file:line, a minimal assembly or C snippet that triggers the bug, expected vs actual bytes if you can, and the fix. Do not modify files. Write a markdown report to stdout.
