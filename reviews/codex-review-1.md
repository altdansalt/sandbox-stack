# Security review: sandboxed execution stack

## Executive summary

The seccomp boundary is effective for syscall confinement: the release binaries are static, contain one `syscall` instruction in `sys3`, and host code after `prctl` only requests `read`, `write`, or `exit`.

However, arbitrary Wasm is **not currently memory-safe after translation**. I found two critical generated-code paths that bypass the runtime’s checked accessors:

1. `memory.init` checks the destination but not the source data-segment range, permitting reads from arbitrary host memory.
2. Active element-segment initialization writes directly into the function table without checking the offset or length, permitting writes outside the table arena.

There is also no `call_indirect` signature check. That is C undefined behavior and prevents treating indirect calls as safely confined, even after the table-write bug is fixed.

## Findings

### Critical: `memory.init` can read outside its data segment

Locations:

- `w2c2/c.c:1530-1559`
- `rt/w2c2_base.h:137`

Generated `memory.init` has this shape:

```c
LOAD_DATA(memory, destination, data_segment + source, length);
```

`LOAD_DATA` performs only:

```c
wasm_check(&(m), destination, length);
memcpy(&m.data[destination], source_pointer, length);
```

The guest controls `source` and `length`, but there is no check that:

```text
source + length <= data_segment_length
```

Consequently, a malicious module can use `memory.init` to copy bytes following the generated static data-segment array into linear memory. It can then send those bytes through `env.write`. This is a host-memory disclosure primitive, even though seccomp still prevents opening files or making other syscalls.

The source-pointer expression can itself run outside the C array before `memcpy` begins, which is also C undefined behavior.

Suggested fix:

- Represent passive data segments as `{ pointer, length, dropped }`.
- Replace `LOAD_DATA` for `memory.init` with a helper accepting the segment and source offset separately.
- Check both ranges using widened arithmetic:

```c
if ((U64)source + length > segment.length)
    trap(trapOutOfBoundsMemory);
wasm_check(memory, destination, length);
```

- Implement `data.drop`, and make subsequent non-empty `memory.init` operations trap.
- Keep a separate checked helper for active data-segment initialization, whose source offset is always zero.

### Critical: active element segments write outside the table arena

Locations:

- `w2c2/c.c:5595-5625`
- Example generated writes: `build/w2c2/guest.c:54542-54560`
- `rt/w2c2_base.h:172-180`

w2c2 emits active element initialization as unchecked native C assignments:

```c
offset = ...;
i->t0.data[offset + 0] = (wasmFunc)&f76;
```

Neither the generated code nor the header verifies that the full element segment fits in `t->size`. The `offset + element_index` expression also uses `U32`, so it can wrap.

A malicious active element segment can therefore write function addresses before or after `wasm_table_arena`. Depending on link layout, this can corrupt runtime globals, instance state, or other host data. This is a host-memory write primitive generated directly from attacker-controlled Wasm.

`wasm_table_get` does not mitigate this because it protects table reads during `call_indirect`, not initialization writes.

Suggested fix:

- Add a checked table initialization helper, for example:

```c
wasmTableInit(table, offset, functions, count);
```

- Validate the whole range with widened arithmetic:

```c
if ((U64)offset + count > table->size)
    trap(trapOutOfBoundsTable);
```

- Do not emit direct `.data[offset + n]` assignments.
- Use equivalent checked helpers for any future `table.init`, `table.copy`, `table.fill`, `table.set`, and `table.grow` support.

### High: `call_indirect` has no function-type check

Locations:

- `rt/w2c2_base.h:167-182`
- Examples: `build/w2c2/guest.c:15754`, `build/w2c2/guest.c:47187`

`TF` verifies the table index and nullness, then casts the stored generic function pointer to the signature requested by the call site:

```c
#define TF(table, index, t) ((t)wasm_table_get(&(table), (index)))
```

Wasm requires `call_indirect` to trap when the selected function’s type does not match the instruction’s expected type. Here, an attacker can call a legitimate generated function through an incompatible C function-pointer type.

Calling through an incompatible function type is undefined behavior in C. On the current x86-64/tcc combination it will often only scramble arguments or return values, but that is not a defensible security property and may become exploitable as generated shapes or compiler behavior change.

Suggested fix:

- Store `{ function_pointer, type_id }` in each table slot.
- Have the generator assign a canonical type ID to every Wasm function signature.
- Pass the expected type ID into `wasm_table_get` and trap on mismatch before casting/calling.
- Avoid using a bare `void (*)(void)` as the complete table representation.

### Medium: table maximum is not checked against the arena

Location: `rt/w2c2_base.h:172-175`

`wasmTableAllocate` checks `size > wasm_table_arena_size`, but accepts an arbitrarily larger `maxSize`:

```c
if (t->data || size > wasm_table_arena_size)
    trap(...);
t->maxSize = maxSize;
```

There is currently no table-growth helper, so this is not directly exploitable by the supplied generated files. It becomes an overflow risk as soon as `table.grow` is supported, and leaves the runtime object claiming capacity that cannot exist.

Suggested fix:

```c
if (t->data ||
    size > maxSize ||
    maxSize > wasm_table_arena_size)
    trap(trapAllocationFailed);
```

### Medium: 4 GiB linear memory wraps `wasmMemory.size` to zero

Locations:

- `rt/w2c2_base.h:103`
- `rt/w2c2_base.h:109-124`

`size` is `U32`, and is calculated as:

```c
m->size = pages * 65536;
```

At the Wasm32 architectural limit of 65,536 pages, the byte size is exactly 2³² and becomes zero in `U32`. All accesses then trap incorrectly.

The configured builds use at most 4,096 pages, so this does not affect the current artifacts. It is nevertheless a latent correctness issue if `ARENA_PAGES` is raised.

Suggested fix:

- Store the current byte length as `U64` or host `size_t`.
- Widen before multiplication:

```c
m->size = (U64)pages * WASM_PAGE_SIZE;
```

- Apply the same rule when validating arena configuration.

### Medium: Wasm effective-address overflow has incorrect semantics

Generated access examples culminate in checked helpers such as:

- `build/w2c2/guest.c:54523-54525`
- Emitter: `w2c2/c.c:1145-1170`, `w2c2/c.c:1295-1318`

w2c2 computes a load/store effective address as `U32 base + constant_offset` before passing it to the checked accessor. That addition can wrap to a small in-bounds address.

This does not escape the arena, because the wrapped address is still checked, but it violates Wasm semantics: the operation should trap when the mathematical effective address exceeds the current memory.

Suggested fix:

- Pass base and static offset separately to the accessor, or compute in `U64`.
- Trap if the full widened `base + offset + access_size` exceeds memory size.

## Host and seccomp audit

### No unintended post-seccomp syscalls found

Relevant locations:

- Seccomp entry: `host/main.c:63-68`
- Guest execution: `host/main.c:68-70`
- Raw syscall wrapper: `host/main.c:19-23`
- Exit path: `host/main.c:24-25`

After successful `PR_SET_SECCOMP`, the host performs:

1. `guestInstantiate`
2. `guest__start`
3. raw `SYS_exit`

The release artifacts inspected were static ELF executables with no dynamic section or unresolved symbols. Disassembly of both:

- `build/rot13/sandbox`
- `build/w2c2/sandbox`

showed exactly one `syscall` instruction, inside the shared `sys3` wrapper. There were no `int 0x80` or `sysenter` sites.

Thus, tcc has not inserted startup, TLS, allocator, stack-protector, or arithmetic-helper syscalls into these binaries. `memcpy`, `memmove`, and `memset` are host-provided loops. Generated `NewChild` can reach `calloc`, but the host implementation traps rather than allocating.

The `DEMO_ESCAPE` configuration at `host/main.c:52-54` deliberately requests `getpid`; it must remain excluded from production builds. Strict seccomp kills it as expected.

Even if memory corruption obtains arbitrary native control flow, strict seccomp should continue to reject other syscalls. The memory-safety findings still permit disclosure/corruption of process memory and invalidate the intended C-level sandbox boundary.

## Guest libc correctness and determinism

### High: allocation-size multiplication overflows in `calloc`

Location: `libc/libc.c:28-31`

```c
void* p = malloc(n * m);
if (p) memset(p, 0, n * m);
```

The multiplication is unchecked. A wrapped product allocates and clears a smaller block than the caller requested. w2c2 uses `calloc(count, element_size)` for attacker-controlled module counts, so a crafted input can turn this into guest-memory corruption during translation.

Although guest-memory corruption remains inside the outer arena once the runtime issues are fixed, it can make the generated output incorrect or non-reproducible.

Suggested fix:

```c
if (m && n > SIZE_MAX / m)
    return NULL;
size_t total = n * m;
```

### High: `fopen` and buffered writes discard failed `realloc` results

Locations:

- `libc/stdio.c:18-35`
- `libc/stdio.c:39-46`

Examples:

```c
f->cap *= 2;
f->buf = realloc(f->buf, f->cap);
```

On failure, the old pointer is lost and execution continues with `NULL`. Capacity doubling and `f->len + total` can also wrap. Similar unchecked allocations occur for `f->name` and the initial buffers.

For a sufficiently large or adversarial Wasm input/output, native w2c2 returns an allocation error or write failure, while guest w2c2 can trap, corrupt its heap, or emit truncated/different C.

Suggested fix:

- Check every allocation.
- Store `realloc` into a temporary.
- Check capacity addition and doubling for overflow.
- Preserve the original object on failure and propagate an error.

### Medium: `realloc` does not implement standard zero-size behavior

Location: `libc/libc.c:33-41`

For `realloc(p, 0)`, the code returns `p` because `0 <= old`. C permits implementation variation around zero-size allocation, so this alone is not necessarily invalid, but it can differ from the native libc and complicate cross-platform behavior.

More importantly, `realloc` trusts the 16-byte-preceding size header without validating that `p` is an allocator result. That is acceptable only if this libc is explicitly treated as unsafe guest code, not as a general memory-safety boundary.

Suggested fix:

- Define and document one intentional zero-size policy.
- Consider allocating zero as a minimum-sized object or returning `NULL`.
- If malformed guest pointers must be diagnosed, track heap bounds and validate alignment/header range.

### Medium: `fread(..., size = 0, ...)` divides by zero

Location: `libc/stdio.c:48-55`

The return statement is:

```c
return total / sz;
```

When `sz == 0`, standard `fread` must return zero, but this implementation divides by zero.

Suggested fix:

```c
if (sz == 0 || n == 0)
    return 0;
```

Also check multiplication overflow in `sz * n`.

### Medium: `fseek` accepts invalid and out-of-range positions

Locations:

- `libc/stdio.c:56-61`
- Consequence: `libc/stdio.c:51`

`fseek` always succeeds and computes:

```c
f->pos = base + (size_t)off;
```

Negative offsets can wrap, invalid `whence` is treated as `SEEK_END`, and positions beyond the buffer are accepted. A later `fread` evaluates `f->len - f->pos`, which can underflow and produce an oversized copy.

The current w2c2 `readFile` path uses only the valid sequence `SEEK_END`, `ftell`, `rewind`, but this FILE layer is not generally correct.

Suggested fix:

- Reject invalid `whence`.
- Compute signed offsets without unsigned conversion.
- Reject negative resulting positions.
- In `fread`, first handle `pos >= len`.
- Decide explicitly whether seeking past end is supported for output streams.

### Low: negative zero-padding in `printf` is incorrect

Locations: `libc/stdio.c:100-117`

The sign is placed inside `body`, while padding is emitted before the body. Thus:

```c
sprintf(buf, "%05d", -12)
```

produces `00-12` instead of `-0012`.

Numeric precision is also parsed but ignored, and the `0` flag is not disabled by `-` or numeric precision as required by standard `printf`.

This does not appear to affect current w2c2 code generation: its zero-padded formats are unsigned hexadecimal (`%02X`, `%08X`, `%016llX`). It can affect diagnostics or future generator changes.

Suggested fix:

- Emit the sign before zero padding.
- Implement numeric precision separately from field width.
- Make `-` override `0`.

### Low: `qsort` is deterministic for current uses, but artificially limited

Location: `libc/stdio.c:159-167`

The stable insertion sort is deterministic, and the important function-hash comparator now has an explicit index tie-break at `w2c2/main.c:107-119`. I found no present output nondeterminism caused by sorting.

However, `qsort` aborts for element sizes above 64 bytes:

```c
char tmp[64];
if (size > sizeof tmp) abort();
```

That is not a conforming general `qsort`. It also lacks overflow checks for `i * size`.

Suggested fix:

- Allocate the temporary element dynamically, or perform byte-wise rotation without a fixed limit.
- Guard index multiplication.
- Keep total-order comparator tie-breakers; do not depend on sort stability for reproducible output.

## TCB reduction opportunities

- The generated guest C is large but is not conceptually a trusted library; the trusted property should instead be established by a small, complete set of checked runtime primitives. Direct element writes and unchecked data-segment pointers currently break that abstraction.
- `w2c2/debug.c` and DWARF machinery are compiled but inactive without libdwarf.
- Thread, WASI, multi-module, cleanup/glob, and platform compatibility paths are unnecessary for the fixed wasm32-to-C build.
- tcc’s in-memory execution, archive/tooling, assembler, and other unused front-end paths can be removed from a purpose-built translator compiler.
- Generated `NewChild`, export metadata, resolver callbacks, free-instance code, and host `calloc` are unnecessary if the sandbox only instantiates one fixed module with three statically linked imports. Removing their generation would reduce both code size and callable surface.
- The FILE abstraction contains substantially more behavior than the translator needs. A fixed stdin-backed input object and framed stdout output object would be smaller and easier to make overflow-safe.

## Recommended remediation order

1. Fix checked data-segment source ranges for `memory.init`.
2. Replace direct element-segment table writes with checked initialization.
3. Add `call_indirect` type IDs and signature validation.
4. Add adversarial regression modules for all three cases.
5. Fix guest-libc allocation and FILE arithmetic.
6. Correct 4 GiB memory accounting and effective-address semantics.
7. Trim unused w2c2/tcc/runtime features only after the safety boundary is complete.