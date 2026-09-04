# Minimal sandboxed execution stack: wasm → C → tcc → seccomp-strict

**Goal:** measure the smallest trusted source surface (tokens, kernel excluded) that turns C source into a process which can only map stdin bytes to stdout bytes.

**Pipeline:** `foo.c` →(clang, untrusted, temporary)→ `foo.wasm` →(w2c2)→ `foo.c'` →(tcc)→ native, run by a tiny host that pre-allocates memory, wires `env.read/write/exit`, then enters `SECCOMP_MODE_STRICT` before calling `_start`.

## Where things stand

| Step | State |
|---|---|
| Environment (clang+lld wasm32, tcc, go, w2c2, wazero, chibicc, tokei, tiktoken) | done |
| Exp 1: cat/rot13 through the full pipeline under seccomp-strict | **done, verified** (strace shows only prctl/read/write/exit; disallowed syscall → SIGKILL) |
| Exp 2: w2c2 compiled to wasm, run inside the sandbox, fixpoint check | **done, verified**: sandboxed w2c2 translating rot13.wasm and w2c2.wasm is byte-identical to native and to wazero; gen-2 translator identical |
| Measurement table (lines + tokens of the TCB) | Exp 2: **39,651 code lines / 421,143 tokens**; tcc is 75% |
| Exp 3: chibicc → wasm (no clang) | pending |

## What's happening right now
Experiment 3: a chibicc derivative that emits wasm directly, to replace the clang leg. A codex review of the trust boundary (host, runtime header, libc) is running in the background; findings land in `reviews/`.

## Findings so far
- w2c2 upstream has **no memory bounds checks** and `call_indirect` has no table bounds/null/signature check. The runtime header was rewritten (164 lines) to add software checks and a static memory arena. See NOTES.md "Sharp edges".
- The host has no libc at all: `tcc -nostdlib -static`, raw syscalls, `SYS_exit` not `exit_group`. strace of a run: `execve, prctl, read, write, exit`.
- Experiment 2 TCB: ~39.7k code lines, ~421k tokens (o200k_base). tcc alone is 315k tokens. The libc grew from 88 to 404 lines to host w2c2 (memory FILEs, integer printf, qsort).
- Two nondeterminism sources removed from w2c2 by patch: float constants printed with `%.17g` (now hex bit patterns), and function emission order depending on qsort stability (comparator now total).
