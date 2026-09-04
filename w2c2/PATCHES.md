# Local patches to vendored w2c2

Upstream commit: see UPSTREAM. Diff against upstream: `diff -ru ~/src/w2c2/w2c2 w2c2` (test files were dropped, not patched).

1. `c.c` `wasmCWriteConstantValue`: f32/f64 constants are always emitted as
   `f32_reinterpret_i32(0x..)` / `f64_reinterpret_i64(0x..)`. Upstream printed
   finite non-negative-zero values with `%.9g` / `%.17g`, which (a) needs a
   correct decimal float printer in the guest libc and (b) is a platform
   nondeterminism source. Hex is exact and identical everywhere.
2. `stringbuilder.c/.h`: removed `stringBuilderAppendF32/F64` (only users of float printf).
3. `main.c`: the "%.2f%%" statistic on stderr in the reference-module path is now an integer message.
4. `main.c` `wasmFunctionIDsCompareHashes`: tie-break on function index. Functions
   are emitted in hash order; identical bodies hash equal, so the emitted order of
   duplicates depended on the platform qsort's stability (glibc mergesort: stable;
   musl smoothsort: not). Now a total order.
5. `main.c` `cleanImplementationFiles`: without glob/Win32 the function is a no-op
   instead of `#error` (the wasm guest has no directory listing).
6. `c.c` (codex review 1): `memory.init` is emitted as `MEMORY_INIT(mem, dest, dN, dN_len, src, len)` and
   the runtime checks `src+len <= dN_len` as well as the destination; every data segment gets a
   `U32 dN_len` variable and `data.drop` (previously "unimplemented") sets it to 0.
7. `c.c` (codex review 1): element-segment initialisation emits `wasm_table_init(&table, offset, count)`
   (traps unless the whole range fits) before the writes, and records a canonical function type id in
   `table.types[]` for each entry. `call_indirect` passes the expected canonical id:
   `TF(table, index, type_id, ctype)`; the runtime traps on mismatch. Canonical id = index of the first
   structurally identical entry of the type section (`wasmCanonicalFunctionTypeIndex`).
6. Mechanical pruning with `unifdef -k -UHAS_PTHREAD -UHAS_LIBDWARF -UHAS_OLD_LIBDWARF -U_WIN32 -UHAS_GLOB -UHAS_LIBGEN -UHAS_STRDUP -UHAS_GETOPT -DHAS_UNISTD=1 -U__APPLE__ -U_MSC_VER -U__cplusplus`
   over every `.c`/`.h`: removes the thread pool, libdwarf line tables (debug.c is now a 15-line stub),
   Windows, glob/libgen/strdup/getopt fallbacks that this build never compiled. Output of the translator is
   byte-identical before and after (checked on rot13.wasm and w2c2.wasm).
