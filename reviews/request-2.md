Review request: a wasm32 backend for chibicc (directory cc/ in this repo).

Context: NOTES.md, section "Experiment 3". cc/codegen.c is new (replaces chibicc's x86-64 codegen.c); cc/main.c is a rewritten driver; cc/parse.c, type.c, tokenize.c, preprocess.c, chibicc.h have small patches (diff against ~/src/chibicc to see them). libc/include/stdarg.h and libc/libc.h have __chibicc__ variants that define the ABI from the C side.

It compiles w2c2 (11k lines of C89) plus a small libc into a wasm module that reproduces native w2c2's output byte for byte, so gross bugs are unlikely; I want a careful review for:
1. Miscompilation risks that the w2c2 test input would not exercise: integer conversion and narrowing rules (cast(), narrow(), gen_expr_as), signed/unsigned division and shifts, comparison of mixed types, pointer arithmetic with 64-bit indices, float<->int conversions, bool semantics, char signedness, ILP32 literal typing in tokenize.c (all combinations of U/L/LL suffixes and hex/decimal), long double as double.
2. ABI consistency: hidden struct-return pointer (parse.c change + codegen), struct parameters copied by callee, varargs slots (caller stores, callee's va_arg reads 8-byte slots; float promoted to double), call_indirect signatures matching definitions, K&R-style empty parameter lists.
3. Control flow lowering: switch with fallthrough, case ranges, nested loops with break/continue inside switch, forward goto blocks, return inside nested blocks with the $sp restore, do/while continue semantics, unreachable code after br, if/else with result types (ND_COND, && and ||).
4. Memory layout: data segment relocations (pointers to globals/functions/strings in static initializers), globals resolved by name, alignment, stack/heap placement, memory min/max pages, and anything that could make output depend on hash-map iteration order or other nondeterminism.
5. Wasm binary encoding correctness (LEB128 signed/unsigned use, section ordering, type dedup, elem/table sizes).
6. Anything that would silently accept unsupported C instead of erroring.

Be concrete: file:line, a minimal C snippet that triggers the bug, and the fix. Do not modify files. Write a markdown report to stdout.
