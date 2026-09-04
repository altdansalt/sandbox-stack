# Review report: x86-64 assembler, ELF writer, and target integration

I found several correctness bugs outside the currently exercised paths. The ELF layout and ordinary rel32 calculation are generally sound, but there are important encoding, parsing, and integration problems.

## High severity

### 1. `mov $imm, %r8w` emits the REX prefix in the wrong position

**Location:** `cc/asm.c:267-272`

Legacy prefixes must precede the REX prefix. The manual `B8+r` encoder currently emits REX first and then `0x66`.

Minimal input:

```asm
.text
.globl _start
_start:
  mov $1, %r8w
  hlt
```

Expected:

```text
66 41 b8 01 00
```

Actual:

```text
41 66 b8 01 00
```

Because REX must be the last prefix before the opcode, the intervening `0x66` makes the preceding `0x41` ineffective. The instruction writes `%ax`, not `%r8w`.

The same bug affects all `mov $imm16, %r8w` through `%r15w`.

**Fix:** emit `0x66` before the computed REX byte, or route this form through a common encoder that understands the opcode-embedded register as REX.B.

---

### 2. High-byte source registers can silently turn into low-byte registers when the memory base requires REX

**Locations:** `cc/asm.c:163-177`, `cc/asm.c:240-244`, `cc/asm.c:281-283`, `cc/asm.c:450-453`

`encode()` rejects a high-byte register only when it is the ModRM r/m operand:

```c
if (e->rm->kind == O_REG && e->rm->high8 && rex != 0x40)
```

It cannot tell whether the ModRM.reg operand was `%ah`/`%ch`/`%dh`/`%bh`.

Minimal input:

```asm
mov %ah, (%r8)
```

Expected: assembler error; `%ah` cannot be encoded in an instruction containing any REX prefix.

Actual:

```text
41 88 20
```

Under REX, ModRM.reg value 4 means `%spl`, so the emitted instruction is effectively:

```asm
mov %spl, (%r8)
```

The same issue affects ALU/test operations with a high-byte register in the ModRM.reg field, for example:

```asm
add %ah, (%r8)
test %ah, (%r12)
mov %bh, (%r13)
```

**Fix:** carry `reg_high8` in `Enc`, or carry an `Op *reg_op` rather than only its number. Reject if either ModRM operand is a high-byte register and any REX bit or forced REX is present.

---

### 3. XMM-to-XMM `movq` is encoded as a GPR-to-XMM move

**Location:** `cc/asm.c:387-390`

Minimal input:

```asm
movq %xmm1, %xmm2
```

Expected:

```text
f3 0f 7e d1
```

Actual:

```text
66 48 0f 6e d1
```

The actual bytes mean `movq %rcx,%xmm2`; the r/m register number is interpreted as a GPR for opcode `66 REX.W 0F 6E`.

The reverse direction between two XMM registers has the same fundamental issue.

**Fix:** handle XMM-to-XMM separately with the SSE2 `F3 0F 7E /r` form. Retain `66 REX.W 0F 6E/7E` only for GPR/m64 ↔ XMM forms, with explicit operand-class validation.

---

### 4. GNU `#` comments emitted by the code generator are not stripped

**Locations:** `cc/codegen_x86.c:708,714,722`; `cc/asm.c:328-339`, `cc/asm.c:536-554`

The code generator emits lines such as:

```asm
mov $1065353216, %eax  # float 1.000000
```

The assembler passes `%eax  # float ...` to `parse_reg()`, which reports it as an unknown register.

Minimal C trigger:

```c
void _start(void) {
  volatile float x = 1.0f;
}
```

Double and long-double literals hit the same parser bug.

**Fix:** remove an unquoted `#` comment from every assembly segment before instruction/directive parsing. Since the input is already generated assembly rather than arbitrary GNU source, stripping from the first `#` is sufficient for the current subset.

---

### 5. A generated semicolon-separated cast string contains a label and instruction in the same segment

**Locations:** `cc/codegen_x86.c:333-336`; `cc/asm.c:573-580`, `cc/asm.c:540-554`

The unsigned-64-to-double cast includes:

```asm
...; jmp 2f; 1: mov %rax,%rdi; ...
```

Semicolon splitting produces this segment:

```asm
1: mov %rax,%rdi
```

`assemble_line()` only recognizes a label when the entire trimmed segment ends in `:`. It consequently treats `1:` as an instruction mnemonic and fails.

Minimal C trigger:

```c
double f(unsigned long x) {
  return x;
}
```

This specifically breaks the large-unsigned conversion path represented by `u64f64`.

**Fix:** let `assemble_line()` consume a leading label and then continue parsing the remainder of the same segment. This should work for both numeric and named labels.

---

### 6. Backward numeric labels are emitted by x86 codegen but unsupported

**Locations:** `cc/codegen_x86.c:681-690`; `cc/asm.c:137`, `cc/asm.c:544`

Only `1f` is recognized specially. `1b` becomes the ordinary symbol name `"1b"` and is eventually reported undefined.

The upstream alloca-copy loop emits:

```asm
1:
  ...
  jmp 1b
```

Minimal C trigger:

```c
void sink(void *);

void f(unsigned long n) {
  sink(__builtin_alloca(n));
}
```

Expected: resolve `1b` to the most recent definition of numeric label 1.

Actual: undefined symbol `1b`.

**Fix:** translate `Nb` to `numeric_name(N, numeric_count[N])`, rejecting it immediately if no earlier definition exists. The current `Nf` handling can remain based on `numeric_count[N] + 1`.

## Medium severity

### 7. Operand classes and widths are not validated consistently, causing wrong-but-valid instructions

**Locations:** `cc/asm.c:209-213`, `cc/asm.c:240-249`, `cc/asm.c:281-287`, `cc/asm.c:319-325`, `cc/asm.c:380-413`, `cc/asm.c:450-472`

`op_size()` selects the first available integer register width but does not ensure all register operands have that width.

Examples silently accepted:

```asm
mov %eax, %rax
add %ax, %eax
test %al, %rax
imul %ax, %rax
```

For example, `mov %eax,%rax` is encoded using the destination’s 64-bit width, effectively becoming `mov %rax,%rax`.

The generic SSE helper likewise does not verify that its register field is actually an XMM register or that the source is an allowed XMM/memory/GPR class. Some malformed forms can be emitted as unrelated valid instructions.

Several SSE handlers also dereference `b` without first requiring `n == 2`; malformed one-operand input can crash rather than produce an assembly diagnostic.

**Fix:** define allowed operand shapes per instruction and verify:

- equal integer register widths where required;
- XMM destinations/sources for scalar SSE arithmetic and conversion forms;
- GPR width implied by `l`/`q` conversion suffixes;
- exact operand count before accessing `a` or `b`;
- memory sizes via the mnemonic suffix where no register supplies one.

---

### 8. Positive 32-bit bit patterns are rejected for 32-bit immediate ALU instructions

**Locations:** `cc/asm.c:206-207`, `cc/asm.c:229-235`

Minimal input:

```asm
addl $0x80000000, %eax
```

Expected encoding:

```text
81 c0 00 00 00 80
```

Actual: `immediate too large`.

For a 32-bit destination, all values representable as either signed or unsigned 32-bit bit patterns should be accepted. The same applies to values through `0xffffffff`.

For 64-bit destinations, the `imm32` is sign-extended, so rejecting positive `0x80000000` is appropriate unless it is interpreted explicitly as the signed value `-2147483648`.

**Fix:** make the range depend on operand size:

- 8-bit field: `INT8_MIN..UINT8_MAX`;
- 16-bit field: `INT16_MIN..UINT16_MAX`;
- 32-bit destination: `INT32_MIN..UINT32_MAX`;
- 64-bit ALU destination: `INT32_MIN..INT32_MAX`.

Choose opcode `83` only for signed-byte-equivalent values `-128..127`.

---

### 9. Immediate range checking is missing or asymmetric for byte/word moves and shifts

**Locations:** `cc/asm.c:266-272`, `cc/asm.c:275-278`, `cc/asm.c:481`

Examples:

```asm
mov $-129, %al
mov $-99999, %al
movw $0x10000, (%rax)
shl $999, %rax
```

These are silently truncated. The byte-register check only rejects values above 255; arbitrarily negative values are accepted. Word and 32-bit direct-register moves have no matching validation.

Hardware masks shift counts, and GNU `as` may warn while emitting a truncated byte, but this assembler promises errors rather than silent misassembly. It should explicitly define and enforce its policy.

**Fix:** centralize immediate-field validation. Accept signed or unsigned values fitting the encoded field’s bit pattern, and reject everything else. For shifts, either enforce `0..255` or deliberately permit only counts in the architectural range for the operand width.

---

### 10. Directive parsers accept malformed values and dangerous sizes

**Locations:** `cc/asm.c:503-531`

Most uses of `strtol()` do not inspect the end pointer or `errno`.

Examples silently accepted or mishandled:

```asm
.byte garbage        # becomes zero
.byte 1junk          # becomes one
.zero -1             # decreases bss_len in .bss
.align 16junk        # accepted
.quad 12junk         # becomes 12
.comm x,-8,0         # invalid size/alignment; alignment zero can divide by zero
```

`.comm` also overwrites an existing symbol without duplicate checking:

```asm
.comm x,8,8
.comm x,16,16
```

**Fix:** use strict numeric parsing with full-token consumption, overflow checks, and directive-specific ranges. Require `.zero` and `.comm` sizes to be nonnegative and alignments to be positive powers of two. Route `.comm` symbol creation through duplicate/common-symbol consistency checks.

---

### 11. SSE and other handlers can ignore extra operands

**Location:** `cc/asm.c:328-339` and SSE dispatch at `cc/asm.c:380-413`

The parser stores at most three operands, and many instruction handlers do not require `n == 2`.

For example:

```asm
addsd %xmm0, %xmm1, %xmm2
```

is encoded as the two-operand form using only the first two operands.

**Fix:** after tokenization, reject a fourth operand explicitly and require the exact count in every handler.

---

### 12. File-scope asm parsing implements only a narrower syntax than statement asm

**Location:** `cc/parse.c:3350-3357`

Statement asm accepts `volatile` and `inline`, but file-scope asm does not. It also accepts only one string token, not adjacent concatenated string literals.

Examples rejected:

```c
__asm__ volatile (".globl x\nx:");
__asm__(".globl x\n"
        "x:");
```

**Fix:** share a parser for the common qualifier/string-literal grammar. If qualifiers are intentionally disallowed at file scope, emit a targeted diagnostic rather than failing at `skip("(")`.

## ELF and relocation findings

### 13. The generated file is not made executable

**Location:** `cc/main.c:88-93`

`fopen(..., "wb")` normally creates the output with mode `0666 & umask`; no execute bit is added. The test scripts compensate with `chmod +x`, but a direct invocation advertised as producing an executable does not produce a runnable file.

**Fix:** after successfully writing a native ELF output, apply an executable mode such as `0777 & ~umask`, or preserve existing mode while adding user execute permission. Do not do this for `-S` or wasm output.

---

### 14. No `PT_GNU_STACK` header is emitted

**Location:** `cc/asm.c:608-619`

The two loadable segments are correctly separated RX and RW; neither is W+X. However, the ELF has no `PT_GNU_STACK` declaration. Loader behavior for an absent stack header is platform-dependent and may result in an executable stack or trigger tooling warnings.

**Fix:** add a third program header of type `PT_GNU_STACK`, flags `PF_R | PF_W`, with zero file/memory size and suitable alignment. Update `e_phnum` to 3.

---

### 15. Core ELF load layout and rel32 math appear correct

I did not find a displacement-origin error in the implemented paths:

- RIP-relative operands subtract displacement size and trailing immediate size at `cc/asm.c:183-186`.
- `call`, `jmp`, and long-form `jcc` subtract four bytes at `cc/asm.c:363-372`.
- Resolution uses the address of the relocation field at `cc/asm.c:599`, so the stored `-4-immediate_size` addend produces the address following the complete instruction.
- rel32 range checks are present.
- `.quad symbol+addend` writes an absolute 64-bit virtual address.
- `p_offset % p_align == p_vaddr % p_align` for both `PT_LOAD` segments.
- The first segment is RX and the second RW; there is no writable+executable load segment.
- `p_filesz <= p_memsz`, and the padding before aligned BSS is included in the RW segment’s zero-filled memory size.
- `_start` is required to exist in `.text`.

There are still parser limitations around symbolic displacement expressions. For example:

```asm
mov foo+8(%rip), %rax
```

treats `"foo+8"` as the symbol name and later reports it undefined. Either support symbol addends here or reject them at parse time with a direct diagnostic.

The ELF also intentionally has no section headers. That is valid for execution, though many object-analysis tools will have limited visibility.

## Target integration findings

### 16. The “empty parameter list means no parameters” change also affects x86 and changes C semantics

**Location:** `cc/parse.c:589-596`

The comment says this is a wasm-backend rule, but it is unconditional. In C, a declaration such as:

```c
int f();
```

uses an unspecified parameter list, not a prototype with zero parameters. The modified parser treats it like `int f(void)` and can reject:

```c
int f();
int g(void) {
  return f(1);
}
```

This is an x86 regression relative to upstream chibicc.

**Fix:** preserve upstream unspecified-parameter behavior for x86, or represent “unspecified” separately and let each backend apply its required calling policy.

---

### 17. Long double is advertised as supported by target types/macros but cannot be assembled

**Locations:** `cc/type.c:24-29`, `cc/preprocess.c:1073`, `cc/codegen_x86.c:718-726`, `cc/asm.c:359,415`

The target reports:

```c
sizeof(long double) == 16
```

and the upstream x86 code generator emits x87 instructions, while the assembler deliberately rejects every x87 instruction.

Minimal trigger:

```c
long double f(void) {
  return 1.0L;
}
```

The failure happens late during assembly rather than as a target capability diagnostic.

Global initialization is worse: `cc/parse.c:1464-1467` stores a `TY_LDOUBLE` initializer through `double *`, writing only an 8-byte IEEE double into a 16-byte object. A long-double global that is never used in generated x87 code can therefore be silently malformed.

**Fix:** choose one consistent model:

- implement the required x87 subset and correct 80-bit-in-16-byte initialization; or
- reject `long double` use during semantic analysis; or
- intentionally define `long double` as an 8-byte alias of double and advertise `__SIZEOF_LONG_DOUBLE__ == 8`.

The current mixture is unsafe.

---

### 18. Predefined size macros are incomplete on both targets

**Location:** `cc/preprocess.c:1060-1088`

The integration removed upstream definitions including:

```c
__SIZEOF_INT__
__SIZEOF_SHORT__
__SIZEOF_LONG_LONG__
__SIZEOF_FLOAT__
__SIZEOF_DOUBLE__
```

Headers often use these independently of pointer width. Their absence can select incorrect fallback paths even though the corresponding sizes are known.

**Fix:** define all invariant size macros outside the target branch, and only branch for long, pointer, size/ptrdiff, and long-double widths.

Also consider `__STDC_HOSTED__`: the x86 target is explicitly “no libc”, so advertising a hosted implementation with value 1 is questionable. A freestanding target should normally report 0 unless the supplied runtime genuinely implements the hosted C library contract.

---

### 19. `opt_fcommon = false` itself is reasonable, but `.comm` handling remains inconsistent

**Locations:** `cc/main.c:6`, `cc/codegen_x86.c:1397-1400`, `cc/asm.c:523-531`

With a single translation unit and no linker, lowering tentative definitions directly into BSS is appropriate and avoids common-symbol merging semantics that the ELF writer cannot provide.

However, file-scope assembly can still introduce `.comm`, whose implementation silently replaces duplicate symbols and has weak validation. Either fully define common-symbol merging rules or reject `.comm` if generated C never needs it.

---

### 20. Wasm-visible parser state is global and not reset for repeated compilation

**Locations:** `cc/parse.c:3344-3356`; similarly assembler globals at `cc/asm.c:10-24,565-568`

This executable currently compiles one translation unit per process, so it does not affect the driver. If these functions are ever reused in-process:

- `toplevel_asm` persists across `parse()` calls;
- assembler sections, symbols, numeric counters, and fixups persist across `assemble_elf()` calls.

That would contaminate the next compilation with duplicate symbols and prior output.

**Fix:** explicitly initialize/reset per-compilation state at the entry points, or encapsulate it in a compilation/assembler context.

## Recommended regression cases

At minimum, add byte-exact or execution tests for:

```asm
mov $1,%r8w
mov %ah,(%r8)
movq %xmm1,%xmm2
addl $0x80000000,%eax
movw $1,(%r12)
movw $1,0(%r13)
mov %r8b,(%r12)
mov (%r13),%r15
sete %sil
sete %dil
```

And C-level tests for:

```c
float literal(void) { return 1.0f; }
double u64_to_double(unsigned long x) { return x; }
void *dynamic_alloca(unsigned long n) { return __builtin_alloca(n); }
int old_style();
int call_old_style(void) { return old_style(1); }
long double unsupported(void) { return 1.0L; }
```

The most urgent fixes are the 16-bit REX ordering, high-byte/REX rejection, XMM-to-XMM `movq`, comment stripping, label-plus-instruction parsing, and `Nb` numeric-label support.