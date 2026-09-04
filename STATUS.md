# Minimal sandboxed execution stack: wasm → C → tcc → seccomp-strict

**Goal:** measure the smallest trusted source surface (tokens, kernel excluded) that turns C source into a process which can only map stdin bytes to stdout bytes.

**Pipeline:** `foo.c` →(clang, untrusted, temporary)→ `foo.wasm` →(w2c2)→ `foo.c'` →(tcc)→ native, run by a tiny host that pre-allocates memory, wires `env.read/write/exit`, then enters `SECCOMP_MODE_STRICT` before calling `_start`.

## Where things stand

| Step | State |
|---|---|
| Environment (clang+lld wasm32, tcc, go, w2c2, wazero, chibicc, tokei, tiktoken) | done |
| Exp 1: cat/rot13 through the full pipeline under seccomp-strict | **done, verified** (strace shows only prctl/read/write/exit; disallowed syscall → SIGKILL) |
| Exp 2: w2c2 compiled to wasm, run inside the sandbox, fixpoint check | **done, verified**: sandboxed w2c2 translating rot13.wasm and w2c2.wasm is byte-identical to native and to wazero; gen-2 translator identical |
| Measurement table (lines + tokens of the TCB) | Exp 2: 39,651 lines / 421,143 tokens. After codex fixes: 422,730. Exp 3 (clang replaced): **45,721 lines / 489,928 tokens**; tcc is 64% |
| Exp 3: chibicc → wasm (no clang) | **done, verified**: chibicc-wasm compiles w2c2+libc; that translator reaches the same byte-identical fixpoint in the sandbox; the compiler itself builds with tcc |

## What's happening right now
All three experiments are done and verified. A codex review of the chibicc wasm backend (`cc/`) is running; the earlier codex review of the sandbox boundary found two real holes (unchecked `memory.init` source range, unchecked element-segment writes) which are fixed and covered by hand-assembled adversarial modules in `tests/`.

## Findings so far
- w2c2 upstream has **no memory bounds checks** and `call_indirect` has no table bounds/null/signature check. The runtime header was rewritten (164 lines) to add software checks and a static memory arena. See NOTES.md "Sharp edges".
- The host has no libc at all: `tcc -nostdlib -static`, raw syscalls, `SYS_exit` not `exit_group`. strace of a run: `execve, prctl, read, write, exit`.
- Experiment 2 TCB: ~39.7k code lines, ~421k tokens (o200k_base). tcc alone is 315k tokens. The libc grew from 88 to 404 lines to host w2c2 (memory FILEs, integer printf, qsort).
- Experiment 3: `cc/` is chibicc with a new wasm backend (1,016 lines) emitting the binary directly. w2c2 compiled by it translates itself byte-identically inside the sandbox. clang is out of the loop; tcc can build every tool in the chain.
- Two nondeterminism sources removed from w2c2 by patch: float constants printed with `%.17g` (now hex bit patterns), and function emission order depending on qsort stability (comparator now total).
