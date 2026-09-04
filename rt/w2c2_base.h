/* Minimal runtime header for w2c2-generated C.
 * Replaces upstream w2c2_base.h. Differences from upstream:
 *  - every load/store is software bounds-checked against the current memory size
 *  - memory lives in one static arena; memory.grow only moves a limit, never allocates
 *  - call_indirect checks table index bounds, null entries and the canonical signature id
 *  - memory.init / data.drop and element-segment initialisation are range-checked
 *  - memory/table maxima are clamped to the host arenas; arenas must stay below 65536 pages
 *  - no libc includes: the host provides memcpy/memmove/memset and trap()
 *  - no threads, atomics, big-endian, WASI, or debug support
 * Only wasm32, little-endian hosts. */
#ifndef W2C2_BASE_H
#define W2C2_BASE_H

typedef unsigned char U8;
typedef signed char I8;
typedef unsigned short U16;
typedef signed short I16;
typedef unsigned int U32;
typedef signed int I32;
typedef unsigned long long U64;
typedef signed long long I64;
typedef float F32;
typedef double F64;
typedef U32 WasmPtr;
typedef unsigned long size_t;
typedef enum bool { false = 0, true = 1 } bool;
#define NULL ((void*)0)

#define W2C2_INLINE __inline__
#define W2C2_LL(x) x ## ll
#define W2C2_LOOP_START
#define NORETURN __attribute__((noreturn))
#define UNUSED __attribute__((unused))
#define INT32_MIN (-2147483647 - 1)
#define INT32_MAX 2147483647
#define INT64_MIN (-9223372036854775807LL - 1)
#define INT64_MAX 9223372036854775807LL
#define UINT32_MAX 4294967295U
#define UINT64_MAX 18446744073709551615ULL

void* memcpy(void* d, const void* s, size_t n);
void* memmove(void* d, const void* s, size_t n);
void* memset(void* d, int c, size_t n);

/* traps */
typedef enum Trap {
    trapUnreachable = 1,
    trapDivByZero,
    trapIntOverflow,
    trapInvalidConversion,
    trapAllocationFailed,
    trapOutOfBoundsMemory,
    trapOutOfBoundsTable,
    trapNullFunction,
    trapIndirectCallTypeMismatch
} Trap;
extern void trap(Trap) NORETURN;
#define TRAP(x) (trap(x), 0)
#define UNREACHABLE TRAP(trapUnreachable)

/* integer arithmetic */
#define DIV_S(ut, min, x, y) \
   (((y) == 0) ? TRAP(trapDivByZero) : ((x) == (min) && (y) == -1) ? TRAP(trapIntOverflow) : (ut)((x) / (y)))
#define REM_S(ut, min, x, y) \
   (((y) == 0) ? TRAP(trapDivByZero) : ((x) == (min) && (y) == -1) ? 0 : (ut)((x) % (y)))
#define I32_DIV_S(x, y) DIV_S(U32, INT32_MIN, (I32)(x), (I32)(y))
#define I64_DIV_S(x, y) DIV_S(U64, INT64_MIN, (I64)(x), (I64)(y))
#define I32_REM_S(x, y) REM_S(U32, INT32_MIN, (I32)(x), (I32)(y))
#define I64_REM_S(x, y) REM_S(U64, INT64_MIN, (I64)(x), (I64)(y))
#define DIVREM_U(op, x, y) (((y) == 0) ? TRAP(trapDivByZero) : ((x) op (y)))
#define DIV_U(x, y) DIVREM_U(/, x, y)
#define REM_U(x, y) DIVREM_U(%, x, y)
#define ROTL(x, y, mask) (((x) << ((y) & (mask))) | ((x) >> (((mask) - (y) + 1) & (mask))))
#define ROTR(x, y, mask) (((x) >> ((y) & (mask))) | ((x) << (((mask) - (y) + 1) & (mask))))
#define I32_ROTL(x, y) ROTL(x, y, 31)
#define I64_ROTL(x, y) ROTL(x, y, 63)
#define I32_ROTR(x, y) ROTR(x, y, 31)
#define I64_ROTR(x, y) ROTR(x, y, 63)

static W2C2_INLINE U32 I32_POPCNT(U32 x) {
    x -= ((x >> 1) & 0x55555555u);
    x = ((x >> 2) & 0x33333333u) + (x & 0x33333333u);
    x = ((x >> 4) + x) & 0x0F0F0F0Fu;
    return (x * 0x01010101u) >> 24;
}
static W2C2_INLINE U64 I64_POPCNT(U64 x) {
    x -= ((x >> 1) & 0x5555555555555555ULL);
    x = ((x >> 2) & 0x3333333333333333ULL) + (x & 0x3333333333333333ULL);
    x = ((x >> 4) + x) & 0x0f0f0f0f0f0f0f0fULL;
    return (x * 0x0101010101010101ULL) >> 56;
}
static W2C2_INLINE U32 I32_CLZ(U32 x) { U32 n = 0; if (!x) return 32; while (!(x & 0x80000000u)) { x <<= 1; n++; } return n; }
static W2C2_INLINE U64 I64_CLZ(U64 x) { U64 n = 0; if (!x) return 64; while (!(x & 0x8000000000000000ULL)) { x <<= 1; n++; } return n; }
static W2C2_INLINE U32 I32_CTZ(U32 x) { U32 n = 0; if (!x) return 32; while (!(x & 1)) { x >>= 1; n++; } return n; }
static W2C2_INLINE U64 I64_CTZ(U64 x) { U64 n = 0; if (!x) return 64; while (!(x & 1)) { x >>= 1; n++; } return n; }

/* float helpers (only what integer-heavy guests need; extend when a guest uses more) */
#define DEFINE_REINTERPRET(name, t1, t2) \
  static W2C2_INLINE t2 name(t1 x) { t2 r; memcpy(&r, &x, sizeof r); return r; }
DEFINE_REINTERPRET(f32_reinterpret_i32, U32, F32)
DEFINE_REINTERPRET(i32_reinterpret_f32, F32, U32)
DEFINE_REINTERPRET(f64_reinterpret_i64, U64, F64)
DEFINE_REINTERPRET(i64_reinterpret_f64, F64, U64)

/* memory: one static arena, sized by the host */
typedef struct wasmMemory { U8* data; U32 size; U32 pages; U32 maxPages; } wasmMemory;
#define WASM_PAGE_SIZE 65536
extern U8 wasm_arena[];
extern const U32 wasm_arena_pages;
static wasmMemory wasm_memory0;

static W2C2_INLINE wasmMemory* wasmMemoryAllocate(U32 initialPages, U32 maxPages, bool shared) {
    if (maxPages > wasm_arena_pages) maxPages = wasm_arena_pages; /* growth past the arena fails with -1 */
    if (shared || wasm_memory0.data || wasm_arena_pages >= 65536 || initialPages > maxPages) trap(trapAllocationFailed);
    wasm_memory0.data = wasm_arena;
    wasm_memory0.pages = initialPages;
    wasm_memory0.size = initialPages * WASM_PAGE_SIZE;
    wasm_memory0.maxPages = maxPages;
    return &wasm_memory0;
}
static W2C2_INLINE void wasmMemoryFree(wasmMemory* m) { (void)m; }
static W2C2_INLINE U32 wasmMemoryGrow(wasmMemory* m, U32 delta) {
    U32 old = m->pages, new_ = old + delta;
    if (new_ < old || new_ > m->maxPages) return (U32)-1;
    /* arena is zero-initialised .bss and never shrinks, so new pages are already zero */
    m->pages = new_;
    m->size = new_ * WASM_PAGE_SIZE;
    return old;
}
static W2C2_INLINE void wasm_check(const wasmMemory* m, U32 addr, U32 n) {
    if ((U64)addr + n > m->size) trap(trapOutOfBoundsMemory);
}
static W2C2_INLINE void wasmMemoryCopy(const wasmMemory* d, const wasmMemory* s, U32 da, U32 sa, U32 n) {
    wasm_check(d, da, n); wasm_check(s, sa, n);
    memmove(d->data + da, s->data + sa, n);
}
static W2C2_INLINE void wasmMemoryFill(const wasmMemory* m, U32 da, U32 v, U32 n) {
    wasm_check(m, da, n);
    memset(m->data + da, (int)v, n);
}
#define LOAD_DATA(m, o, i, s) (wasm_check(&(m), (o), (s)), memcpy(&((m).data[o]), (i), (s)))
/* memory.init: both the destination range and the source range within the (possibly dropped) segment are checked */
static W2C2_INLINE void wasm_memory_init(const wasmMemory* m, U32 dest, const U8* seg, U32 seg_len, U32 src, U32 n) {
    if ((U64)src + n > seg_len) trap(trapOutOfBoundsMemory);
    wasm_check(m, dest, n);
    memcpy(m->data + dest, seg + src, n);
}
#define MEMORY_INIT(m, o, seg, seg_len, src, n) wasm_memory_init(&(m), (o), (seg), (seg_len), (src), (n))

#define DEFINE_LOAD(name, t1, t2, t3) \
    static W2C2_INLINE t3 name(wasmMemory* m, WasmPtr a) { t1 r; wasm_check(m, a, sizeof r); memcpy(&r, &m->data[a], sizeof r); return (t3)(t2)r; }
#define DEFINE_STORE(name, t1, t2) \
    static W2C2_INLINE void name(wasmMemory* m, WasmPtr a, t2 v) { t1 w = (t1)v; wasm_check(m, a, sizeof w); memcpy(&m->data[a], &w, sizeof w); }
DEFINE_LOAD(i32_load, U32, U32, U32)
DEFINE_LOAD(i64_load, U64, U64, U64)
DEFINE_LOAD(f32_load, F32, F32, F32)
DEFINE_LOAD(f64_load, F64, F64, F64)
DEFINE_LOAD(i32_load8_s, I8, I32, U32)
DEFINE_LOAD(i64_load8_s, I8, I64, U64)
DEFINE_LOAD(i32_load8_u, U8, U32, U32)
DEFINE_LOAD(i64_load8_u, U8, U64, U64)
DEFINE_LOAD(i32_load16_s, I16, I32, U32)
DEFINE_LOAD(i64_load16_s, I16, I64, U64)
DEFINE_LOAD(i32_load16_u, U16, U32, U32)
DEFINE_LOAD(i64_load16_u, U16, U64, U64)
DEFINE_LOAD(i64_load32_s, I32, I64, U64)
DEFINE_LOAD(i64_load32_u, U32, U64, U64)
DEFINE_STORE(i32_store, U32, U32)
DEFINE_STORE(i64_store, U64, U64)
DEFINE_STORE(f32_store, F32, F32)
DEFINE_STORE(f64_store, F64, F64)
DEFINE_STORE(i32_store8, U8, U32)
DEFINE_STORE(i32_store16, U16, U32)
DEFINE_STORE(i64_store8, U8, U64)
DEFINE_STORE(i64_store16, U16, U64)
DEFINE_STORE(i64_store32, U32, U64)

/* tables: one static array, sized by the host */
typedef void (*wasmFunc)(void);
typedef struct wasmTable { wasmFunc* data; U32* types; U32 size, maxSize; } wasmTable;
extern wasmFunc wasm_table_arena[];
extern U32 wasm_table_types_arena[];
extern const U32 wasm_table_arena_size;
static W2C2_INLINE void wasmTableAllocate(wasmTable* t, U32 size, U32 maxSize) {
    if (maxSize > wasm_table_arena_size) maxSize = wasm_table_arena_size;
    if (t->data || size > maxSize) trap(trapAllocationFailed);
    t->data = wasm_table_arena; t->types = wasm_table_types_arena; t->size = size; t->maxSize = maxSize;
}
static W2C2_INLINE void wasmTableFree(wasmTable* t) { (void)t; }
/* element segment init: the whole [offset, offset+count) range must fit */
static W2C2_INLINE void wasm_table_init(const wasmTable* t, U32 offset, U32 count) {
    if ((U64)offset + count > t->size) trap(trapOutOfBoundsTable);
}
static W2C2_INLINE wasmFunc wasm_table_get(const wasmTable* t, U32 i, U32 expected_type) {
    if (i >= t->size) trap(trapOutOfBoundsTable);
    if (!t->data[i]) trap(trapNullFunction);
    if (t->types[i] != expected_type) trap(trapIndirectCallTypeMismatch);
    return t->data[i];
}
#define TF(table, index, type_id, t) ((t)wasm_table_get(&(table), (index), (type_id)))

typedef struct wasmFuncExport { wasmFunc func; char* name; } wasmFuncExport;
typedef struct wasmModuleInstance {
    wasmFuncExport* funcExports;
    void* (*resolveImports)(const char* module, const char* name);
    struct wasmModuleInstance* (*newChild)(struct wasmModuleInstance* self);
} wasmModuleInstance;
void* calloc(size_t n, size_t m); /* referenced by generated NewChild; host makes it trap */

#endif
