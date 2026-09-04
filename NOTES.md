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

## Experiment 2: fixpoint the translator (done, verified)

### What was built
- `w2c2/`: vendored w2c2 core (upstream commit in `w2c2/UPSTREAM`), 5 small patches listed in `w2c2/PATCHES.md`. Most important: float constants are always emitted as `f32_reinterpret_i32(0x..)`/`f64_reinterpret_i64(0x..)` instead of `%.9g`/`%.17g`, and the function-order comparator got an index tie-break (see nondeterminism below).
- `libc/` grew from 88 to 404 code lines (1092 → 5362 tokens): `libc/include/*.h` (12 tiny headers), `libc/stdio.c` (memory FILEs, integer-only printf with width/zero-pad/l/ll/z, `qsort` as a stable insertion sort, `strtoul`, `strerror`, `strncmp/strncpy/strchr/strrchr`, `exit/abort/assert`). `malloc` gained a 16-byte size header so `realloc` copies exactly. Floats in printf abort on purpose.
- I/O model for a guest that wants files: `fopen(_, "r")` slurps stdin once (fseek/ftell/rewind work on the buffer); each `fopen(_, "w")` buffers in memory and on `fclose` writes `@@FILE <name> <len>\n` + bytes to stdout. `tools/split.py` turns the stream back into files. `chdir` and `remove` are no-ops. argv is baked in at link time (`-DGUEST_ARGV='"w2c2","guest.wasm","guest.c"'`) so no new host import was needed.
- Build: `make build/w2c2/sandbox` — vendored w2c2 sources → clang → `w2c2.wasm` (124,666 bytes) → native w2c2 → `guest.c` (681,823 bytes, 1 file) → tcc → `sandbox` (700 KB static binary, 256 MiB bss arena via `-DARENA_PAGES=4096`).

### Results
| run | input | output vs native w2c2 (same patched source, clang -O2) | time |
|---|---|---|---|
| sandbox w2c2 (seccomp strict) | rot13.wasm | byte-identical (guest.h 749 B, guest.c 3446 B) | 2 ms |
| sandbox w2c2 (seccomp strict) | w2c2.wasm | byte-identical (guest.h 6065 B, guest.c 681,823 B) | 0.21 s |
| wazero interpreter (control) | rot13.wasm | byte-identical | — |
| wazero interpreter (control) | w2c2.wasm | byte-identical | 1.16 s |
| gen-2: tcc-compile the C that the *sandboxed* w2c2 emitted for w2c2.wasm | — | binary byte-identical to gen-1 sandbox; its output on rot13.wasm and w2c2.wasm identical | — |

`strace -c` of the sandboxed w2c2 translating rot13.wasm: 1 prctl, 2 read, 19 write, then exit. Nothing else.

### Nondeterminism sources found
1. **Float constant printing** (`%.9g`/`%.17g` via the platform printf). Removed by patch: hex bit patterns. Would otherwise have required a correctly-rounded decimal printer in the libc and made the output depend on libc quality.
2. **qsort stability.** w2c2 emits functions in SHA1-of-body order; identical bodies compare equal, and the emitted order of such duplicates then depended on whether the platform `qsort` is stable (glibc: mergesort, stable in practice; musl: smoothsort, not). Patched the comparator to tie-break on function index. Duplicate-body counts for the inputs used are recorded just below; the outputs matched even before the patch because both glibc's qsort and the guest libc's insertion sort happened to be stable.
3. **Nothing else observed.** No uninitialised-memory dependence surfaced (the guest libc's bump allocator returns fresh zero pages; native glibc malloc does not, and outputs still match). Custom-section warnings go to stderr and are not compared.

### Sharp edges
- w2c2's `cleanImplementationFiles` is `#error` without glob/Win32; patched to a no-op.
- w2c2 must be built with `HAS_UNISTD=1` for the `chdir` declaration; everything else (`HAS_PTHREAD`, `HAS_GLOB`, `HAS_LIBGEN`, `HAS_STRDUP`, `HAS_GETOPT`) off, so w2c2 uses its own `getopt_impl.h`, `compat.c` basename/dirname, and `str.h` strdup.
- The generated C for w2c2.wasm needed one more macro from the runtime header than rot13 did: `W2C2_LL` (int64 literal suffix). Nothing else; still no float arithmetic helpers needed.
- Memory: w2c2.wasm is linked with `--max-memory=256MiB`; the host arena is sized to match. Actual use for translating w2c2.wasm is a few MiB (bump allocator, no free).

### Measurement (Experiment 2 TCB)
| component | files | code lines | comment lines | tokens (o200k_base) | bytes |
|---|---:|---:|---:|---:|---:|
| w2c2 core (vendored, patched; excludes tests and upstream w2c2_base.h) | 45 | 12015 | 346 | 96840 | 403480 |
| w2c2 runtime header (rt/w2c2_base.h, rewritten) | 1 | 165 | 14 | 2909 | 8338 |
| tcc (x86_64 Linux build inputs, commit 62c30a4a) | 20 | 27008 | 3126 | 315053 | 1055247 |
| minimal libc (libc/) | 16 | 404 | 12 | 5362 | 16410 |
| host (host/main.c) | 1 | 59 | 4 | 979 | 2719 |
| **total** | | **39651** | | **421143** | |

Delta vs Experiment 1: +316 libc lines (+4270 tokens); w2c2 core shrank slightly from the float-printing removal. Total 421,143 tokens; tcc is 74.8%.
