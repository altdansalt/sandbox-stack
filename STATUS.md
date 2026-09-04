# Minimal sandboxed execution stack: wasm → C → tcc → seccomp-strict

**Goal:** measure the smallest trusted source surface (tokens, kernel excluded) that turns C source into a process which can only map stdin bytes to stdout bytes.

**Pipeline:** `foo.c` →(clang, untrusted, temporary)→ `foo.wasm` →(w2c2)→ `foo.c'` →(tcc)→ native, run by a tiny host that pre-allocates memory, wires `env.read/write/exit`, then enters `SECCOMP_MODE_STRICT` before calling `_start`.

## Where things stand

| Step | State |
|---|---|
| Environment (clang+lld wasm32, tcc, go, w2c2, wazero, chibicc, tokei, tiktoken) | done |
| Exp 1: cat/rot13 through the full pipeline under seccomp-strict | **done, verified** (strace shows only prctl/read/write/exit; disallowed syscall → SIGKILL) |
| Exp 2: w2c2 compiled to wasm, run inside the sandbox, fixpoint check | pending |
| Measurement table (lines + tokens of the TCB) | Exp 1 numbers in: **39,363 code lines / 416,988 tokens**; tcc is 76% |
| Exp 3: chibicc → wasm (no clang) | pending |

## What's happening right now
Experiment 2: compiling w2c2 itself to wasm. This needs the libc to grow (stdio/printf subset, qsort, strtoul, real realloc) and a patch to w2c2 so float constants are emitted as hex bit patterns instead of `%.17g`.

## Findings so far
- w2c2 upstream has **no memory bounds checks** and `call_indirect` has no table bounds/null/signature check. The runtime header was rewritten (164 lines) to add software checks and a static memory arena. See NOTES.md "Sharp edges".
- The host has no libc at all: `tcc -nostdlib -static`, raw syscalls, `SYS_exit` not `exit_group`. strace of a run: `execve, prctl, read, write, exit`.
- Experiment 1 TCB: ~39k code lines, ~417k tokens (o200k_base). tcc alone is 315k tokens.
