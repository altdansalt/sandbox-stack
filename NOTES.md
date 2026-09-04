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

## Codex review 1: findings and fixes

A read-only codex review (`reviews/codex-review-1.md`) audited the trust boundary. It confirmed the seccomp confinement (static binaries, one `syscall` site in `sys3`, only read/write/exit after `prctl`) and found memory-safety holes in the *generated-code* path — w2c2's emitter bypassed the runtime's checked accessors for three wasm constructs. All fixes below are verified by adversarial modules in `tests/` (`make test`).

### Findings and what changed
| # | sev | finding | fix |
|---|---|---|---|
| 1 | critical | `memory.init` checked the destination but not the source range within the data segment → read past the segment array into host memory, exfiltratable via `env.write` | emitter now writes `MEMORY_INIT(mem,dest,dN,dN_len,src,len)`; runtime `wasm_memory_init` checks `(U64)src+len<=dN_len` and the destination (`w2c2/c.c`, `rt/w2c2_base.h`). Each segment gets a `U32 dN_len`; `data.drop` (was "unimplemented") sets it to 0. |
| 2 | critical | active element segments emitted `t0.data[offset+n]=&fN;` with no bounds check → function-pointer writes outside the table arena | emitter now emits `wasm_table_init(&t0,offset,count)` (traps `trapOutOfBoundsTable` unless `(U64)offset+count<=size`) before the writes (`w2c2/c.c`, `rt/w2c2_base.h`) |
| 3 | high | `call_indirect` had no signature check → call through an incompatible C function-pointer type (UB) | every wasm function type gets a canonical id (first structurally-equal type-section entry); the table carries a parallel `U32 types[]`; each `TF(table,index,type_id,ctype)` traps `trapIndirectCallTypeMismatch` on mismatch (`w2c2/c.c`, `rt/w2c2_base.h`, `host/main.c` adds `wasm_table_types_arena[]`) |
| 4 | medium | `wasmTableAllocate` accepted `maxSize` larger than the arena | clamp `maxSize` to the arena and trap if `size>maxSize` |
| 5 | medium | `size = pages*65536` wraps to 0 at 65536 pages (U32) | `wasmMemoryAllocate` traps if `wasm_arena_pages>=65536`; documented |
| 6 | high | `calloc(n,m)` multiplication overflow | returns NULL when `n>SIZE_MAX/m` (`libc/libc.c`) |
| 7 | high | `fopen`/`fwrite` discarded failed `realloc`, continued with NULL; capacity doubling could wrap | new `grow()` helper: overflow-checked, allocation failure is a fatal `_exit`, never silent (`libc/stdio.c`) |
| 8 | medium | `fread(sz=0)` divided by zero; `fseek` accepted invalid whence / negative positions (later underflow in `fread`) | `fread` returns 0 for `sz==0||n==0||pos>=len`; `fseek` rejects bad whence and negative results (`libc/stdio.c`) |
| 9 | low | `printf` put the sign after zero-padding (`%05d` of -12 → `00-12`) | sign emitted before zero padding; verified `-0012` natively (`libc/stdio.c`) |

### Adversarial modules (`tests/mkwasm.py`, hand-assembled wasm; `make test`)
| module | attack | sandbox result | wazero control |
|---|---|---|---|
| positive | write "ok\n", exit 0 | exit 0 | exit 0 |
| oob_load | `i32.load` at 0x7fffff00 | trap 6 (OOB memory), exit 106 | traps (exit 3) |
| mem_init_oob | `memory.init` src=0 len=100 from a 3-byte passive segment | trap 6, exit 106 | traps (exit 3) |
| elem_oob | active element segment at offset 5 into a 2-slot table | trap 7 (OOB table), exit 107 | **exit 0 — accepts it** |
| callind_typemismatch | `call_indirect` as `(i32,i32,i32)->i32` through a `(i32)->i32` entry | trap 9 (type mismatch), exit 109 | traps (exit 3) |

Note on `elem_oob`: wazero v1.9.0 rejects an out-of-bounds active *data* segment (verified separately: "data[0]: out of bounds memory access") but silently accepts an out-of-bounds active *element* segment, even with a declared table max. So for this one case the sandbox is **stricter** than the wazero control. The module is kept in the suite as a sandbox trap check; w2c2 accepts and translates it (it does not validate segment bounds itself).

### Fixpoint re-verified after all fixes
Regenerated from scratch (`rm -rf build/cmp build/w2c2 build/gen2 ...`): sandboxed w2c2 translating rot13.wasm and w2c2.wasm is byte-identical to native w2c2 (same patched source) and to the wazero control; the gen-2 binary (tcc-compiling the C the sandboxed w2c2 emitted) is byte-identical to gen-1 and produces identical output. `strace -c`: 1 prctl, 2 read, 19 write, exit — unchanged.

### Not fixed (deliberate), with reasons
- **Effective-address wrap semantics** (`base+offset` in U32 can wrap to a small in-bounds address): still a spec deviation, but the wrapped address is bounds-checked, so it cannot escape the arena. Fixing means threading base/offset separately through every accessor; deferred as a semantics (not safety) issue.
- **qsort 64-byte element limit**: `abort()`s above 64 bytes. Nothing in w2c2 sorts elements that large (largest is `WasmFunctionID`, 24 bytes). Documented in a comment rather than generalized, to keep the libc small.
- **realloc validating the block header**: the guest libc is inside the arena and is not itself a safety boundary (the runtime accessors are). A malformed guest pointer corrupts only guest memory, which stays inside the seccomp+arena box.

### Measurement (after review 1)
| component | files | code lines | tokens (o200k_base) |
|---|---:|---:|---:|
| w2c2 core (vendored, patched) | 45 | 12090 | 97775 |
| w2c2 runtime header (rt/w2c2_base.h) | 1 | 179 | 3275 |
| tcc (x86_64 Linux, 62c30a4a) | 20 | 27008 | 315053 |
| minimal libc (libc/) | 16 | 420 | 5637 |
| host (host/main.c) | 1 | 60 | 990 |
| **total** | | **39757** | **422730** |

Delta vs Experiment 2 (421,143 tokens): +1587 tokens for the three memory-safety fixes plus the libc hardening (+935 w2c2 core, +366 runtime header, +275 libc, +11 host). The safety of arbitrary wasm now rests on the runtime accessors, not on trusting the generated C.

## Experiment 3: C → wasm without clang (done, verified)

### What was built
`cc/`: a chibicc derivative (upstream commit in `cc/UPSTREAM`) whose x86-64 `codegen.c` is replaced by a wasm32 backend that emits the binary directly (no wat, no assembler, no linker, no WABT). Single translation unit; the guest and the libc are `#include`d into one file (`build/cc/<name>.c`).

Backend design (all in `cc/codegen.c`, 1,016 lines):
- Every C local lives on a shadow stack in linear memory (`$sp` global, `$fp` local), so `&local` works; expressions evaluate on the wasm operand stack. Value classes: ≤4-byte ints and pointers → i32, 8-byte ints → i64, float → f32, double/long double → f64. Struct-typed expressions evaluate to the struct's address, as in chibicc.
- ILP32 (`long` and pointers 4 bytes, `long long` 8), matching clang's wasm32, so the same libc headers work. This needed a distinct `ty_llong`/`ty_ullong` in `type.c`, the `long long` cases in `declspec`, and a rewrite of integer-literal typing in `tokenize.c` (which now tracks the `LL` suffix).
- ABI: struct params passed by pointer and copied by the callee; every struct return gets a hidden first pointer parameter (chibicc only did this above 16 bytes; one-line change in `parse.c`); variadic functions take one trailing i32 pointing at 8-byte argument slots that the caller pushes on the shadow stack; `va_list` is a `char*` (`libc/include/stdarg.h` has the `__chibicc__` variant). Function pointers are table indices (index+1; 0 is null); every function is in the table.
- Control flow: `if`/`for`/`while`/`do` map to `block`/`loop`/`br`; `switch` uses the nested-block trick (one block per case, cases end their own block), so **fallthrough works for free**. `goto` is supported only forward to a label at the top level of the function body (a block per label opened at function entry); every `goto` in w2c2 is a forward `goto fail;`, so nothing had to be patched. `return` is a `br` to an outer block with the value in a local, so the epilogue restores `$sp` once.
- Imports: an undefined function named `__env_X` is imported as `env.X`; any other undefined function is an error. Exports: `_start` by name, and `memory`.
- Unsupported on purpose: `alloca`, VLAs, bitfields, TLS, atomics, `asm`, statement expressions, labels-as-values, backward or nested `goto`, structs as variadic args. Each is a clear error, not silent miscompilation.
- Predefined macros changed to `__wasm32__`/`__ILP32__`/little-endian; the x86-64/Linux ones are gone.
- Driver `cc/main.c` is 80 lines: `-I`, `-D`, `-E`, `-o`, `-mstack=`, `-mmaxpages=`. No subprocesses.

### Results
| check | result |
|---|---|
| cat, rot13, evil, grow, escape compiled by chibicc-wasm, then w2c2 → tcc → seccomp | all behave exactly as the clang builds (rot13 output, OOB trap 6, grow refusals, fd-3 refusal) |
| w2c2 + libc as one TU (11,977 lines) compiled by chibicc-wasm | compiles; 225,143-byte wasm (clang -O2: 127,207) |
| that w2c2.wasm in the sandbox translating rot13.wasm | byte-identical to native w2c2 and to wazero |
| that w2c2.wasm in the sandbox translating **itself** | byte-identical to native w2c2 and to wazero (1,358,188 bytes of C) |
| gen-2 (tcc-compile the C the sandboxed translator emitted for itself) | binary identical to gen-1 |
| chibicc-wasm compiled by **tcc** instead of clang | emits a byte-identical w2c2.wasm |
| `make test` (probes + adversarial modules) | all pass |

So the clang leg is gone: C source → chibicc-wasm → w2c2 → tcc → seccomp-strict, and every tool in that chain can itself be compiled by tcc. Cost: the unoptimised code is ~16× slower (sandboxed self-translation 3.4 s vs 0.21 s) and 2× larger; irrelevant for the experiment.

### Sharp edges found
- chibicc keeps every declaration of a global as its own object and binds names to the latest one, so after a header is included twice, references point at an `extern` declaration rather than the definition. The backend resolves globals by name (`global_def` map) and lays out one storage location per name.
- chibicc's `equal()` reads `tok->len` bytes of a shorter string literal (harmless on glibc, flagged by ASan); made bounds-safe.
- `ND_MEMZERO`/`ND_NULL_EXPR` nodes have no type; the value-class lookup treats a missing type as "no value".
- My own printf used `goto` into a label inside a `switch`; rewritten without `goto` (the backend's forward-goto rule is real).
- The grow probe assumed clang's 16 initial pages; rewritten to read the initial size. chibicc-wasm places a 1 MiB stack after the data, so its modules start with more pages.
- w2c2's sources include `w2c2_base.h` (for its own type names), whose endianness detection needs `__LITTLE_ENDIAN__`; added to the predefined macros.

### Measurement (Experiment 3)
| component | files | code lines | comment lines | tokens (o200k_base) | bytes |
|---|---:|---:|---:|---:|---:|
| w2c2 core (vendored, patched; excludes tests and upstream w2c2_base.h) | 45 | 12090 | 353 | 97775 | 407730 |
| w2c2 runtime header (rt/w2c2_base.h, rewritten) | 1 | 179 | 18 | 3275 | 9622 |
| tcc (x86_64 Linux build inputs, commit 62c30a4a) | 20 | 27008 | 3126 | 315053 | 1055247 |
| minimal libc (libc/) | 16 | 449 | 17 | 5908 | 18188 |
| host (host/main.c) | 1 | 60 | 4 | 990 | 2759 |
| chibicc-wasm (cc/, replaces clang) | 10 | 5935 | 677 | 66927 | 212587 |
| **total** | | **45721** | | **489928** | |

Tokens changed versus upstream chibicc (line-level diff, o200k_base tokens of removed and added lines):

| chibicc file | upstream tokens | ours | tokens removed | tokens added |
|---|---:|---:|---:|---:|
| chibicc.h | 2820 | 2847 | 0 | 27 |
| codegen.c | 14777 | 12608 | 14618 | 12364 |
| main.c | 5484 | 807 | 5461 | 655 |
| parse.c | 26607 | 26616 | 79 | 88 |
| preprocess.c | 9629 | 9598 | 158 | 127 |
| tokenize.c | 6113 | 6245 | 149 | 281 |
| type.c | 2454 | 2493 | 90 | 129 |
| **total changed** | | | **20555** | **13671** |

Reading: the backend is a full replacement of `codegen.c` (12.4k tokens written) and a rewrite of the driver (0.7k); the front end changed by ~600 tokens across `parse.c`, `type.c`, `tokenize.c`, `preprocess.c`, `chibicc.h`. Unchanged: `hashmap.c`, `strings.c`, `unicode.c`.

The full TCB with clang replaced is 489,928 tokens; tcc is 64% of it. Note that the TCB now contains *two* C compilers (chibicc-wasm at 67k tokens, tcc at 315k), which is the obvious next target: either teach chibicc-wasm's front end an x86-64 backend for the host side (chibicc upstream already has one; then tcc goes away, ~315k → ~15k), or run w2c2's output through chibicc-wasm again and interpret it. Not started, per the brief.

## Codex review 2 (chibicc wasm backend): findings and fixes

Request: `reviews/request-2.md`; report: `reviews/codex-review-2.md`. Seven findings, all fixed; regression tests in `tests/cc/` (run by `make test`, compiled by chibicc-wasm and executed under wazero).

| # | severity | finding | fix |
|---|---|---|---|
| 1 | high | static `long double` initialisers serialised as integers (`parse.c` only handled float/double) | `TY_LDOUBLE` treated like double; test `ldouble.c` |
| 2 | high | `switch` case values truncated to `int` in the parser | `int64_t` temporaries and `Node.begin/end`; test `case64.c` |
| 3 | high | `int f();` was modelled as variadic, so a later prototyped definition got a phantom varargs parameter (compiler crash or ABI mismatch) | empty parameter list now means "no parameters"; a call with arguments through such a declaration is rejected ("too many arguments"); `is_compatible` distinguishes `long` from `long long` by size; test `knr.c` (must be rejected) |
| 4 | medium | `_LP64`, `linux`, `unix` still predefined | removed |
| 5 | medium | out-of-range integer literals silently mistyped; `strtoul` overflow unchecked | `strtoull` with `ERANGE` check; unsuffixed decimal above `INT64_MAX` is an error |
| 6 | medium | `_Atomic`/`_Thread_local`/bitfield declarations accepted silently when unreferenced | rejected at parse time |
| 7 | low | `-mstack`/`-mmaxpages` unchecked; layout arithmetic in `int` | range-checked options; 64-bit layout arithmetic; data+stack must fit `-mmaxpages` |
| — | note | float constants and relocations written with host byte order | now serialised explicitly little-endian in the backend; `parse.c` still writes initialised scalars through host pointers (little-endian IEEE host assumed; documented, not fixed) |

Codex found no problems in the cast/narrow rules, signedness of div/rem/shift/compare, struct-return and struct-parameter ABI, varargs, switch fallthrough, loop/continue nesting, `$sp` restore on return, short-circuit blocks, section encoding, or hash-order determinism. `tests/cc/varargs.c` and `switchfall.c` pin those down anyway.

After the fixes the Experiment 3 fixpoint was re-run from scratch (rot13 and self, sandbox and wazero, gen-2): unchanged, byte-identical. `make test`: all pass. TCB: 45,735 lines / 490,402 tokens; tokens changed vs upstream chibicc: 20,706 removed, 14,297 added.

## Experiment 4: tcc out of the TCB (done, verified)

### What was built
The host side (the no-libc `host/main.c` plus the w2c2-generated C) is now compiled by chibicc's own x86-64 backend, so one compiler front end with two backends replaces both clang and tcc.
- `cc/codegen_x86.c`: upstream chibicc's x86-64 codegen, almost verbatim (301 tokens added, 183 removed): it prints into a buffer instead of a FILE, drops `.loc` lines, appends file-scope `asm` blocks, and hands the text to the assembler. No binutils anywhere.
- `cc/asm.c` (633 lines, new): a two-pass assembler for exactly the GNU-syntax subset chibicc emits (about 30 integer mnemonics, the movs/movz family, setcc/jcc, push/pop, scalar SSE, `rep stosb`, `syscall`, `hlt`) and a static ELF64 writer (two PT_LOAD segments, no sections, no relocations, entry `_start`). Every symbol resolves inside the image; `sym@GOTPCREL(%rip)` is encoded as `lea sym(%rip)`. Unknown instructions, x87 (long double), TLS and atomics are errors.
- Target selection: `-mx86` flips the front end to LP64 (`set_target_lp64`: 8-byte long/pointers, 16-byte long double), selects LP64 predefined macros, the x86 struct-return rule (hidden pointer only above 16 bytes) and the 136-byte x87-style `__va_area__`; wasm stays ILP32. `parse.c` gained file-scope `asm("...")` (used by the host for `_start` and the syscall stub, since chibicc has no asm operand constraints). `host/main.c` has a `__chibicc__` branch for those two pieces.
- Build: `build/x86/<guest>/sandbox` = `chibicc-wasm -mx86 -I. -Irt -Ibuild/<guest> -o sandbox host_tu.c` where the TU is `#include "host/main.c"` + the generated `guest.c`. Compile time for the 1.36 MB w2c2 guest: 2.5 s.

### Verification
| check | result |
|---|---|
| cat, rot13, evil, grow, escape with the chibicc-compiled host | identical stdout and exit codes to the tcc-compiled hosts (`tests/run-x86.sh`) |
| five adversarial modules (OOB load, memory.init source OOB, element segment OOB, call_indirect type mismatch, positive control) | identical traps/exit codes to the tcc hosts |
| escape demo host (`-DDEMO_ESCAPE`, issues getpid) | SIGKILL, as with tcc |
| strace of a rot13 run | `execve, prctl, read, write, read, exit`, unchanged |
| w2c2 compiled by chibicc-wasm, hosted by chibicc-x86, translating rot13.wasm and itself | byte-identical to native w2c2 and to wazero; gen-2 (the sandboxed translator's own output for itself, compiled by chibicc-x86) is binary-identical |
| same with the clang-built w2c2.wasm | byte-identical |
| assembler differential against GNU `as` (`tests/asm-diff.sh`, developer-only, binutils not in TCB) | 994,926 instructions for the w2c2 host TU: identical instruction streams (the only difference by construction is GOTPCREL `mov` → `lea`, which the check maps as equivalent) |

Timing: sandboxed self-translation takes 17.6 s with the chibicc-compiled host (3.4 s with tcc, 0.21 s when both stages were clang -O2). Two layers of unoptimised codegen; irrelevant for the measurement.

### Sharp edges found
- `hlt` in user mode raises SIGSEGV with `si_code=SI_KERNEL`; that was the symptom of my first bug (a GOTPCREL `mov` encoded as a real load, so `call *%r10` jumped into `sys3`'s instruction bytes and returned into `_start`'s `hlt`). `strace -i` gives the faulting IP without a debugger.
- chibicc rejects `void f(T) __attribute__((noreturn))`-style prototypes when `__attribute__` is not defined away; the runtime header now defines it away under `__chibicc__`, as the libc headers already did.
- My ELF has no section headers, so `objdump -d` shows nothing; disassemble the raw text with `objdump -D -b binary -m i386:x86-64 --adjust-vma`.
- `"\x7fELF"` in C swallows the following `E` as a hex digit; use octal.
- The C89 output of w2c2 exercises only: mov/lea/push/pop/movsxd/add/sub/cmp/and/or/xor/shl/shr/sar/neg/imul/div/setcc/jmp/je/jne/jbe/call/ret/rep stosb. The scalar SSE support is untested by these inputs (no float arithmetic in the translator); it is written from the manual and encodes identically to `as` on the cast-table snippets only where those appear.

### Measurement (Experiment 4)
| component | files | code lines | comment lines | tokens (o200k_base) | bytes |
|---|---:|---:|---:|---:|---:|
| w2c2 core (vendored, patched; excludes tests and upstream w2c2_base.h) | 45 | 12090 | 353 | 97775 | 407730 |
| w2c2 runtime header (rt/w2c2_base.h, rewritten) | 1 | 182 | 18 | 3289 | 9673 |
| tcc (x86_64 Linux build inputs, commit 62c30a4a) | 20 | 27008 | 3126 | 315053 | 1055247 |
| minimal libc (libc/) | 16 | 449 | 17 | 5908 | 18188 |
| host (host/main.c) | 1 | 65 | 5 | 1087 | 3034 |
| chibicc (cc/: front end, wasm backend, x86-64 backend, assembler, ELF writer) | 12 | 7837 | 840 | 94146 | 288634 |
| **total with tcc as host compiler** | | **47631** | | **517258** | |
| **total with chibicc x86-64 as host compiler (tcc excluded)** | | **20623** | | **202205** | |

Tokens changed versus upstream chibicc: 20,964 removed, 26,582 added (of which `asm.c` is 11,379 new and `codegen_x86.c` differs by 301/183).

So the complete trusted source for "C text in, stdin→stdout process out" is now **20,623 code lines / 202,205 tokens**: w2c2 (48%), chibicc with both backends and the assembler (47%), and 5% for the runtime header, libc and host. clang, tcc, binutils and every libc are outside the boundary; the kernel is excluded by the brief. Remaining levers, not started: prune w2c2 (`debug.c`, threads, WASI, multi-module: probably a third of it) and drop the wasm backend's unused float paths.
