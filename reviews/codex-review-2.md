# Review: `cc/` wasm32 backend

I found seven actionable issues: three miscompilations/ABI failures, one target-definition bug, and three cases where invalid or unsupported input is silently accepted or produces an invalid module.

## 1. High: static `long double` initializers are serialized as integers

Location: `cc/parse.c:1460-1474`

`long double` is represented as an 8-byte IEEE-754 double, but global initialization only recognizes `TY_FLOAT` and `TY_DOUBLE`. A `TY_LDOUBLE` initializer falls through to `eval2()`, whose floating result is converted to `int64_t`, then written as integer bytes.

```c
long double x = 1.5L;

int _start(void) {
  return x == 1.5L ? 0 : 1;
}
```

The stored bits become integer `1` (`0x0000000000000001`) rather than the double representation of `1.5`.

Fix: treat `TY_LDOUBLE` exactly like `TY_DOUBLE` when serializing globals:

```c
if (ty->kind == TY_DOUBLE || ty->kind == TY_LDOUBLE) {
  *(double *)(buf + offset) = eval_double(init->expr);
  return cur;
}
```

For host-independent output, serialize the IEEE-754 bits explicitly in little-endian form instead of writing through a host `double *`.

## 2. High: 64-bit `switch` case values are truncated to 32 bits

Locations:

- `cc/parse.c:1605-1621`
- `cc/codegen.c:662-669`
- `cc/chibicc.h:269-271`

Although `Node.begin` and `Node.end` are `long`, the parser first stores each constant in an `int`:

```c
long long f(long long x) {
  switch (x) {
  case 0x100000000LL:
    return 1;
  case 0:
    return 2;
  }
  return 3;
}
```

`0x100000000LL` becomes zero at `cc/parse.c:1605`, so the generated comparison is against zero. Case ranges have the same problem.

Fix: make the parser temporaries `int64_t`:

```c
int64_t begin = const_expr(&tok, tok->next);
int64_t end;
```

Prefer also changing `Node.begin/end` to `int64_t`, rather than host `long`, to make their width independent of the compiler host. Case values should ultimately be converted to the promoted type of the controlling expression before comparison.

Duplicate cases created by truncation—and duplicate cases generally—should also be diagnosed.

## 3. High: an old-style declaration followed by a prototyped definition breaks the ABI

Locations:

- `cc/parse.c:586-635`
- `cc/parse.c:3209-3246`
- `cc/codegen.c:782-805`

An empty parameter list is represented as `is_variadic = true`. On redeclaration, `function()` reuses the existing `Obj` but never updates or reconciles `fn->ty`.

```c
int f();

int f(int x) {
  return x;
}

int _start(void) {
  return f(7);
}
```

Consequences:

- `fn->params` is built from the definition and contains `x`.
- `fn->ty` remains the old “variadic, zero fixed parameters” type.
- `gen_function()` reserves a variadic wasm parameter because `fn->ty->is_variadic` is true.
- No `fn->va_area` was created, because the definition itself is not variadic.
- `cc/codegen.c:805` dereferences `fn->va_area`, typically crashing the compiler.

Even if that crash is avoided, call sites parsed through `int f()` use the backend’s private varargs-pointer convention, while the definition expects ordinary positional parameters. Thus a call could pass the address of an 8-byte vararg area where the callee expects `x`.

Fix options:

1. Simplest and safest: reject prototype-less function types and old-style definitions with a clear diagnostic.
2. Full support: preserve “unspecified parameters” as a state distinct from `...`, implement default argument promotions as positional wasm parameters, and reconcile every declaration with the eventual definition. Existing call ASTs may also need their effective signature updated after parsing the translation unit.

Do not model `f()` as variadic; C’s unspecified-parameter form is not equivalent to `f(...)` and does not have a hidden `va_list` argument.

Related issue: `is_compatible()` at `cc/type.c:58-63` considers every signed `TY_LONG` compatible regardless of size. After introducing distinct 32-bit `long` and 64-bit `long long` objects with the same `TypeKind`, compatibility checks must compare size/rank too.

## 4. Medium: `_LP64` is predefined for an ILP32 target

Location: `cc/preprocess.c:1062-1081`

The compiler simultaneously defines `_LP64` and `__ILP32__`.

```c
#ifdef _LP64
typedef unsigned long word_t;
#else
typedef unsigned long long word_t;
#endif

int check[sizeof(word_t) == 8 ? 1 : -1];
```

Under this compiler, the `_LP64` branch is selected even though `long` and pointers are four bytes. Real-world headers use `_LP64` to choose layouts and typedefs, so this can cause ABI or structure-layout mismatches.

Fix: remove `_LP64`. Consider also removing the bare `linux` and `unix` macros at `cc/preprocess.c:1096-1097`, unless Linux compatibility is intentional; the emitted module is not itself a Linux target.

## 5. Medium: out-of-range integer constants are silently assigned incorrect types/values

Locations: `cc/tokenize.c:357-419`

Decimal constants for which no signed type can represent the value are still assigned `long long`, and `strtoul()` overflow is not checked.

```c
long long a = 9223372036854775808;
long long b = 9223372036854775808LL;
unsigned long long c = 18446744073709551616ULL;
```

Problems:

- Unsuffixed decimal `9223372036854775808` has no valid type in the target’s standard integer candidate list. It is classified as signed `long long` and stored as `INT64_MIN`.
- Decimal `9223372036854775808LL` also does not fit its requested signed type but is accepted as `long long`.
- Values above `UINT64_MAX` become whatever saturation/error behavior the host `strtoul()` supplies, because `errno` is ignored.
- Parsing depends on host `unsigned long` width, even though the target ABI is fixed.

The ordinary in-range ILP32 candidate ordering for `U`, `L`, `UL`, `LL`, and hexadecimal constants otherwise looks correct.

Fix:

- Parse into a checked `uint64_t` accumulator, or reset/check `errno == ERANGE`.
- Select types by explicit maximum values.
- Diagnose when no permitted target type can represent the literal.
- In particular, decimal signed `LL` constants must be rejected above `INT64_MAX`; nondecimal `LL` may fall through to `unsigned long long`.

## 6. Medium: atomic and TLS declarations can be silently accepted

Locations:

- `cc/parse.c:402-443`
- `cc/parse.c:3276-3290`
- `cc/codegen.c:363-369`
- `cc/codegen.c:535-567`

The backend documentation says atomics and TLS are unsupported, but errors are only issued for certain generated operations or when a TLS object’s address is actually emitted.

```c
_Atomic int x;

int _start(void) {
  x = 1;
  return x;
}
```

This compiles to ordinary non-atomic wasm loads and stores. There is no diagnostic for the lost atomic semantics.

An unused TLS declaration can also be accepted:

```c
_Thread_local int x;
int _start(void) { return 0; }
```

Fix: reject unsupported type/storage features independently of reachability:

- Reject any object or member whose type has `is_atomic` during parsing or a complete program validation pass.
- Reject every `Obj` with `is_tls`, even when unreferenced.
- Apply the same early-validation principle to bitfields if the intent is that they are wholly unsupported; currently unused bitfields and some static initialization paths are accepted.

## 7. Low: memory options can produce malformed modules or overflow layout arithmetic

Locations:

- `cc/main.c:40-41`
- `cc/codegen.c:849-873`
- `cc/codegen.c:963-966`

Both options are parsed with unchecked `atoi()`, and layout is calculated in signed `int`.

Examples:

```sh
cc -mmaxpages=70000 -o out.wasm input.c
cc -mstack=-1 -o out.wasm input.c
cc -mstack=2147483647 -o out.wasm input.c
```

WebAssembly 1.0 memory32 limits cannot exceed 65,536 pages. A value above that is emitted rather than rejected. Negative values are passed into unsigned LEB encoding or layout calculations, and a large stack can overflow `stack_top`/`heap_base`, potentially allowing stack/data overlap or producing an invalid initial limit.

Fix:

- Parse with `strtoull()` plus complete syntax/range checks.
- Require `0 <= max_pages <= 65536`.
- Require a positive, suitably aligned stack size.
- Calculate data, stack, heap, and page counts in `uint64_t`.
- Reject any address above `UINT32_MAX`, arithmetic overflow, or `min_pages > max_pages` before narrowing.

## Other reviewed areas

I did not find a backend-specific correctness problem in these paths:

- `cast()`/`narrow()` correctly canonicalize `_Bool`, sign-extend signed `char`/`short`, and mask unsigned narrow types.
- Signedness selection for integer division, remainder, relational comparisons, and right shift follows the converted left operand.
- Float-to-integer wasm traps only occur for cases where C conversion is undefined; valid in-range conversions use the appropriate signed/unsigned opcode.
- Fixed arguments are cast by the parser before code generation, and variadic `float` arguments are promoted to `double`.
- The hidden struct-return pointer is consistently first in signatures, calls, and definitions.
- Struct parameters are passed by address and copied into the callee’s frame.
- `do`/`while` continuation targets the condition, and loop `continue`/`break` lookup remains correct through nested switches.
- The nested-block switch lowering provides ordinary fallthrough. Unsupported case-label nesting is diagnosed.
- Returns branch to the common epilogue, so `$sp` is restored once even from nested control flow.
- `ND_COND`, `&&`, and `||` use correctly typed wasm blocks and preserve short-circuit behavior.
- Type-section deduplication, section ordering, table offset/indexing, and element counts are internally consistent.
- Module numbering and global layout are driven by source-list traversal rather than hash-map iteration, so I found no hash-order nondeterminism.

One portability caveat remains: floating constants, initialized scalar data, and relocations are copied using host-native byte representation in `cc/codegen.c:148-149`, `cc/codegen.c:881-888`, and `cc/parse.c:1395-1417`. Output is deterministic on the current little-endian IEEE-754 host, but a genuinely host-independent compiler should serialize all target values explicitly in little-endian form.