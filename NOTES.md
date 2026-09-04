# NOTES

Lab notebook: numbers, failures, assumptions. Newest sections at the bottom.

## 2026-09-04 environment
- Ubuntu 24.04, 8 cores, 31 GiB RAM.
- clang 18 + lld (apt) with wasm32 target. tcc 0.9.27+git20200814 (apt). go 1.27.1.
- tokei 12.1.2 (prebuilt binary; not in apt). tiktoken in `.venv` (encoding: `o200k_base`, GPT-4o style BPE).
- Clones (shallow) under `~/src`: w2c2, wazero, chibicc.

## Experiment 1: close the loop (done, verified)

### What was built
- `libc/`: 88-line wasm32 libc with imports `env.read`, `env.write`, `env.exit`; bump allocator over `memory.grow`; `free` is a no-op.
- `guest/cat.c`, `guest/rot13.c`, plus probes `evil.c` (out-of-bounds load), `grow.c` (memory.grow limits), `escape.c` (asks host for fd 3).
- `rt/w2c2_base.h`: rewritten runtime header (164 lines) — see sharp edges below.
- `host/main.c`: 59-line x86_64 host, no libc at all (`tcc -nostdlib -static -nostdinc`), raw syscalls via inline asm, `_start` in file-scope asm.
- `control/main.go`: wazero interpreter harness with the same three imports (control only, not TCB).

### Verified
- `echo "Hello, World" | build/rot13/sandbox` → `Uryyb, Jbeyq`; cat works.
- `strace` of a run shows exactly: `execve, prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT), read, write, read, exit`. Nothing else, ever: the binary is static and has no libc, so there is no brk/mmap/arch_prctl at startup either.
- `evil` guest (load from 0xFFFFFFF0) → `trap: 6` (out-of-bounds memory), exit 106. wazero control gives "out of bounds memory access" for the same .wasm.
- `grow` guest: grow 16→116 pages succeeds, grow past max (256) returns -1, new pages are zero and writable, one byte past `size` traps. No realloc anywhere.
- `escape` guest asking for fd 3: correct host returns -1. Host built with `-DDEMO_ESCAPE` (fd 3 → `getpid`) is `killed by SIGKILL` under strace. That is the kernel enforcing strict mode.

### Sharp edges found
1. **w2c2 has no memory bounds checks at all.** Upstream `DEFINE_LOAD` is a bare `memcpy(&r, &mem->data[addr], n)`. There is no "bounds-check mode" to choose; the only safety upstream is whatever the guest's own code does. Resolved by rewriting the runtime header: every load/store/bulk op/data-segment init checks `(U64)addr + n > size` and traps. Software checks only, no signal handlers, no guard pages.
2. **Effective-address wrap.** w2c2 emits `base + offsetU` in U32 arithmetic, so a base near 2^32 with a nonzero static offset wraps to a small address instead of trapping (spec deviation). It stays inside the checked region, so it is a semantics difference, not a sandbox hole. Not fixed.
3. **memory.grow used realloc.** Replaced: memory is a static `.bss` arena sized by `-DARENA_PAGES` in the host; grow only moves the size limit within `maxPages`. Newly exposed pages are zero because the arena is bss and nothing ever shrinks it (assumption: no reuse; holds because there is one instance and no free).
4. **call_indirect had no table bounds check, no null check, no signature check.** Added bounds and null checks in `TF()`. Signature check is still missing (calling through a mismatched pointer type is UB in C, but stays inside the process). Not fixed yet.
5. **`exit_group` is fatal under strict mode.** glibc's `exit()`/`_exit()` use `exit_group`. The host uses raw `SYS_exit` (60) and has no libc. Verified with strace (`exit(0)`).
6. **Does w2c2 output compile under tcc?** Yes, clean, with the rewritten header. The upstream header pulls in `<math.h>`, `<assert.h>`, pthread, and `__builtin_*`; none of that is needed for these guests. Float support in the rewritten header is limited to reinterpret casts so far; float arithmetic macros will be added only when a guest needs them.
7. **Does the tcc binary touch brk at startup?** Not applicable: host is `-nostdlib -static`, tcc does not link libtcc1 with `-nostdlib`, and strace shows no brk/mmap. Generated code uses `calloc` in the (unused) `NewChild` path; host defines `calloc` to trap.
8. **Module naming.** w2c2 derives the C identifiers from the *input* filename, so every guest is copied to `build/<name>/guest.wasm` before translation, giving a fixed `guestInstance`/`guest__start` API for the host.

### Measurement (Experiment 1 TCB)
tokei 12.1.2 code lines (comments/blanks excluded) and tiktoken `o200k_base` tokens of raw file contents. Kernel and clang excluded.

| component | files | code lines | comment lines | tokens (o200k_base) | bytes |
|---|---:|---:|---:|---:|---:|
| w2c2 core (vendored, excludes tests and upstream w2c2_base.h) | 45 | 12044 | 345 | 96967 | 403896 |
| w2c2 runtime header (rt/w2c2_base.h, rewritten) | 1 | 164 | 14 | 2897 | 8311 |
| tcc (x86_64 Linux build inputs, commit 62c30a4a) | 20 | 27008 | 3126 | 315053 | 1055247 |
| minimal libc (libc/) | 2 | 88 | 4 | 1092 | 3469 |
| host (host/main.c) | 1 | 59 | 4 | 979 | 2719 |
| **total** | | **39363** | | **416988** | |

Notes on what is counted:
- tcc: the files the tinycc Makefile lists for an x86_64 build (`CORE_FILES` + `x86_64_FILES`) plus the headers they include (`elf.h`, `stab.h`, `stab.def`, `i386-asm.h`, `i386-tok.h`). `tccrun.c` (in-memory execution) and `tcctools.c` (ar/impdef) are compiled in but not used here; they could be excluded from a trimmed build. `lib/` (libtcc1) is excluded because `-nostdlib` does not link it.
- w2c2 core: all non-test `.c`/`.h` as compiled by upstream's Makefile. `debug.c` (DWARF line tables, ~800 lines) is compiled but inert without libdwarf; threads and WASI-related code are compiled but unused. Pruning is a later step.
- tcc is 76% of the tokens. The next-largest lever after Experiment 3 is trimming tcc (drop tccrun/tcctools/asm).
