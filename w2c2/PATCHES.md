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
