// wasm32 backend for chibicc. Replaces the x86-64 codegen.c.
//
// Model: every C local lives in linear memory on a shadow stack (so its
// address can be taken); expressions evaluate on the wasm operand stack.
// Value classes: integers/pointers of size <= 4 are i32, 8-byte integers
// are i64, float is f32, double and long double are f64. Struct-typed
// expressions evaluate to the struct's address (i32), as in chibicc.
//
// ABI (single translation unit, so only self-consistency matters):
//  - params are passed as wasm values; struct params as a pointer to the
//    caller's object, copied by the callee into its own frame;
//  - a function returning a struct takes a hidden first i32 pointer
//    (parse.c creates that local for every struct return);
//  - a variadic function takes one extra trailing i32: a pointer to the
//    variadic arguments, stored by the caller in 8-byte slots on the
//    shadow stack; va_list is a char*.
//  - function pointers are table indices (index+1, 0 is null).
//
// Control flow: if/for/while/do/switch (with fallthrough) map to wasm
// blocks. goto is supported only forward, to a label at the top level of
// the function body. No alloca, VLA, bitfields, TLS, atomics, asm,
// statement expressions, or labels-as-values.
#include "chibicc.h"

int opt_stack_size = 1 << 20;
int opt_max_pages = 256;

// ---- byte buffers ----
typedef struct { uint8_t *data; int len, cap; } Buf;

static void buf_u8(Buf *b, int v) {
  if (b->len == b->cap) {
    b->cap = b->cap ? b->cap * 2 : 256;
    b->data = realloc(b->data, b->cap);
  }
  b->data[b->len++] = (uint8_t)v;
}
static void buf_bytes(Buf *b, void *p, int n) {
  for (int i = 0; i < n; i++) buf_u8(b, ((uint8_t *)p)[i]);
}
static void buf_uleb(Buf *b, uint64_t v) {
  do { int c = v & 0x7f; v >>= 7; buf_u8(b, v ? c | 0x80 : c); } while (v);
}
static void buf_sleb(Buf *b, int64_t v) {
  for (;;) {
    int c = v & 0x7f; v >>= 7;
    bool done = (v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40));
    buf_u8(b, done ? c : c | 0x80);
    if (done) return;
  }
}
static void buf_name(Buf *b, char *s) { buf_uleb(b, strlen(s)); buf_bytes(b, s, strlen(s)); }
static void buf_buf(Buf *b, Buf *src) { buf_bytes(b, src->data, src->len); }
static void buf_section(Buf *out, int id, Buf *content) {
  buf_u8(out, id); buf_uleb(out, content->len); buf_buf(out, content);
}

// ---- value classes ----
enum { VC_NONE, VC_I32, VC_I64, VC_F32, VC_F64 };
static const uint8_t valtype[] = { 0, 0x7f, 0x7e, 0x7d, 0x7c };

static int vclass(Type *ty) {
  if (!ty) return VC_NONE; // ND_NULL_EXPR / ND_MEMZERO carry no type
  switch (ty->kind) {
  case TY_VOID: return VC_NONE;
  case TY_FLOAT: return VC_F32;
  case TY_DOUBLE: case TY_LDOUBLE: return VC_F64;
  case TY_STRUCT: case TY_UNION: case TY_ARRAY: case TY_FUNC: case TY_PTR: case TY_VLA: return VC_I32;
  default: return ty->size == 8 ? VC_I64 : VC_I32;
  }
}

// ---- opcodes ----
enum {
  OP_UNREACHABLE = 0x00, OP_BLOCK = 0x02, OP_LOOP = 0x03, OP_IF = 0x04, OP_ELSE = 0x05, OP_END = 0x0b,
  OP_BR = 0x0c, OP_BR_IF = 0x0d, OP_RETURN = 0x0f, OP_CALL = 0x10, OP_CALL_INDIRECT = 0x11,
  OP_DROP = 0x1a, OP_SELECT = 0x1b,
  OP_LOCAL_GET = 0x20, OP_LOCAL_SET = 0x21, OP_LOCAL_TEE = 0x22, OP_GLOBAL_GET = 0x23, OP_GLOBAL_SET = 0x24,
  OP_I32_LOAD = 0x28, OP_I64_LOAD = 0x29, OP_F32_LOAD = 0x2a, OP_F64_LOAD = 0x2b,
  OP_I32_LOAD8_S = 0x2c, OP_I32_LOAD8_U = 0x2d, OP_I32_LOAD16_S = 0x2e, OP_I32_LOAD16_U = 0x2f,
  OP_I32_STORE = 0x36, OP_I64_STORE = 0x37, OP_F32_STORE = 0x38, OP_F64_STORE = 0x39,
  OP_I32_STORE8 = 0x3a, OP_I32_STORE16 = 0x3b,
  OP_MEMORY_SIZE = 0x3f, OP_MEMORY_GROW = 0x40,
  OP_I32_CONST = 0x41, OP_I64_CONST = 0x42, OP_F32_CONST = 0x43, OP_F64_CONST = 0x44,
  OP_I32_EQZ = 0x45, OP_I32_EQ = 0x46, OP_I32_NE = 0x47, OP_I32_LT_S = 0x48, OP_I32_LT_U = 0x49,
  OP_I32_LE_S = 0x4c, OP_I32_LE_U = 0x4d,
  OP_I64_EQZ = 0x50, OP_I64_EQ = 0x51, OP_I64_NE = 0x52, OP_I64_LT_S = 0x53, OP_I64_LT_U = 0x54,
  OP_I64_LE_S = 0x57, OP_I64_LE_U = 0x58,
  OP_F32_EQ = 0x5b, OP_F32_NE = 0x5c, OP_F32_LT = 0x5d, OP_F32_LE = 0x5f,
  OP_F64_EQ = 0x61, OP_F64_NE = 0x62, OP_F64_LT = 0x63, OP_F64_LE = 0x65,
  OP_I32_ADD = 0x6a, OP_I32_SUB = 0x6b, OP_I32_MUL = 0x6c, OP_I32_DIV_S = 0x6d, OP_I32_DIV_U = 0x6e,
  OP_I32_REM_S = 0x6f, OP_I32_REM_U = 0x70, OP_I32_AND = 0x71, OP_I32_OR = 0x72, OP_I32_XOR = 0x73,
  OP_I32_SHL = 0x74, OP_I32_SHR_S = 0x75, OP_I32_SHR_U = 0x76,
  OP_I64_ADD = 0x7c, OP_I64_SUB = 0x7d, OP_I64_MUL = 0x7e, OP_I64_DIV_S = 0x7f, OP_I64_DIV_U = 0x80,
  OP_I64_REM_S = 0x81, OP_I64_REM_U = 0x82, OP_I64_AND = 0x83, OP_I64_OR = 0x84, OP_I64_XOR = 0x85,
  OP_I64_SHL = 0x86, OP_I64_SHR_S = 0x87, OP_I64_SHR_U = 0x88,
  OP_F32_NEG = 0x8c, OP_F32_ADD = 0x92, OP_F32_SUB = 0x93, OP_F32_MUL = 0x94, OP_F32_DIV = 0x95,
  OP_F64_NEG = 0x9a, OP_F64_ADD = 0xa0, OP_F64_SUB = 0xa1, OP_F64_MUL = 0xa2, OP_F64_DIV = 0xa3,
  OP_I32_WRAP_I64 = 0xa7, OP_I32_TRUNC_F32_S = 0xa8, OP_I32_TRUNC_F32_U = 0xa9,
  OP_I32_TRUNC_F64_S = 0xaa, OP_I32_TRUNC_F64_U = 0xab,
  OP_I64_EXTEND_I32_S = 0xac, OP_I64_EXTEND_I32_U = 0xad,
  OP_I64_TRUNC_F32_S = 0xae, OP_I64_TRUNC_F32_U = 0xaf, OP_I64_TRUNC_F64_S = 0xb0, OP_I64_TRUNC_F64_U = 0xb1,
  OP_F32_CONVERT_I32_S = 0xb2, OP_F32_CONVERT_I32_U = 0xb3, OP_F32_CONVERT_I64_S = 0xb4, OP_F32_CONVERT_I64_U = 0xb5,
  OP_F32_DEMOTE_F64 = 0xb6,
  OP_F64_CONVERT_I32_S = 0xb7, OP_F64_CONVERT_I32_U = 0xb8, OP_F64_CONVERT_I64_S = 0xb9, OP_F64_CONVERT_I64_U = 0xba,
  OP_F64_PROMOTE_F32 = 0xbb,
  OP_I32_EXTEND8_S = 0xc0, OP_I32_EXTEND16_S = 0xc1,
  OP_PREFIX_FC = 0xfc, // memory.copy = fc 0a 00 00, memory.fill = fc 0b 00
};

// ---- module state ----
static Buf types;          // type section entries
static int ntypes;
static Buf *type_entries;  // each entry's bytes, for dedup
static int type_cap;

static HashMap func_index; // name -> (long)index + 1
static Obj **funcs;        // by index
static int nfuncs, nimports;
static HashMap referenced; // function name -> referenced

static int data_base = 1024;
static int data_end;       // end of initialised + zero globals
static int stack_top;      // initial $sp
static int heap_base;

static Buf code;           // current function body
static Obj *current_fn;
static int frame_size;
static int loc_fp, loc_t32, loc_t32b, loc_t64, loc_tf32, loc_tf64, loc_rv;

// control stack
typedef struct { char *label; int id; } Ctl;
static Ctl ctl[512];
static int ctl_len;
static int ctl_id_counter;
#define RET_LABEL ".ret"

static void gen_expr(Node *node);
static void gen_stmt(Node *node);
static void gen_addr(Node *node);

// ---- emit helpers ----
static void op(int o) { buf_u8(&code, o); }
static void op_u(int o, uint64_t v) { buf_u8(&code, o); buf_uleb(&code, v); }
static void i32c(int64_t v) { buf_u8(&code, OP_I32_CONST); buf_sleb(&code, (int32_t)v); }
static void i64c(int64_t v) { buf_u8(&code, OP_I64_CONST); buf_sleb(&code, v); }
static void buf_le(Buf *b, uint64_t v, int n) { for (int i = 0; i < n; i++) buf_u8(b, (v >> (8 * i)) & 0xff); }
static void f32c(float f) { uint32_t u; memcpy(&u, &f, 4); buf_u8(&code, OP_F32_CONST); buf_le(&code, u, 4); }
static void f64c(double d) { uint64_t u; memcpy(&u, &d, 8); buf_u8(&code, OP_F64_CONST); buf_le(&code, u, 8); }
static void memarg(int o, int align_log2) { buf_u8(&code, o); buf_uleb(&code, align_log2); buf_uleb(&code, 0); }
static void memory_copy(void) { op(OP_PREFIX_FC); buf_uleb(&code, 10); buf_u8(&code, 0); buf_u8(&code, 0); }
static void memory_fill(void) { op(OP_PREFIX_FC); buf_uleb(&code, 11); buf_u8(&code, 0); }

static int push_ctl(char *label) {
  if (ctl_len == 512) error("control nesting too deep");
  ctl[ctl_len].label = label;
  ctl[ctl_len].id = ctl_id_counter++;
  ctl_len++;
  return ctl[ctl_len - 1].id;
}
static void pop_ctl(void) { ctl_len--; }
static int depth_of_label(char *label, Token *tok) {
  for (int i = ctl_len - 1; i >= 0; i--)
    if (ctl[i].label && !strcmp(ctl[i].label, label))
      return ctl_len - 1 - i;
  error_tok(tok, "unsupported control flow: no enclosing block for label %s "
            "(only forward goto to a top-level label is supported)", label);
}
static int depth_of_id(int id) {
  for (int i = ctl_len - 1; i >= 0; i--)
    if (ctl[i].id == id)
      return ctl_len - 1 - i;
  unreachable();
}
static void block(char *label, int blocktype) { op(OP_BLOCK); buf_u8(&code, blocktype); push_ctl(label); }
static int loop(void) { op(OP_LOOP); buf_u8(&code, 0x40); return push_ctl(NULL); }
static void if_(int blocktype) { op(OP_IF); buf_u8(&code, blocktype); push_ctl(NULL); }
static void end(void) { op(OP_END); pop_ctl(); }
static void br_label(char *label, Token *tok) { op_u(OP_BR, depth_of_label(label, tok)); }
static void br_if_label(char *label, Token *tok) { op_u(OP_BR_IF, depth_of_label(label, tok)); }

// ---- function signatures ----
static int add_type_entry(Buf *e) {
  for (int i = 0; i < ntypes; i++)
    if (type_entries[i].len == e->len && !memcmp(type_entries[i].data, e->data, e->len))
      return i;
  if (ntypes == type_cap) {
    type_cap = type_cap ? type_cap * 2 : 64;
    type_entries = realloc(type_entries, type_cap * sizeof(Buf));
  }
  type_entries[ntypes] = *e;
  buf_buf(&types, e);
  return ntypes++;
}

static bool is_struct(Type *ty) { return ty->kind == TY_STRUCT || ty->kind == TY_UNION; }

static int sig_index(Type *fty) {
  Buf e = {0};
  buf_u8(&e, 0x60);
  int n = 0;
  Buf p = {0};
  if (is_struct(fty->return_ty)) { buf_u8(&p, 0x7f); n++; }
  for (Type *t = fty->params; t; t = t->next) { buf_u8(&p, valtype[vclass(t)]); n++; }
  if (fty->is_variadic) { buf_u8(&p, 0x7f); n++; }
  buf_uleb(&e, n); buf_buf(&e, &p);
  int rc = is_struct(fty->return_ty) ? VC_NONE : vclass(fty->return_ty);
  if (rc == VC_NONE) buf_u8(&e, 0);
  else { buf_u8(&e, 1); buf_u8(&e, valtype[rc]); }
  return add_type_entry(&e);
}

// ---- function references (to decide which undefined functions become imports) ----
static void mark_refs(Node *n) {
  for (; n; n = n->next) {
    if (n->kind == ND_VAR && n->var->is_function)
      hashmap_put(&referenced, n->var->name, (void *)1);
    mark_refs(n->lhs); mark_refs(n->rhs); mark_refs(n->cond); mark_refs(n->then);
    mark_refs(n->els); mark_refs(n->init); mark_refs(n->inc); mark_refs(n->body);
    mark_refs(n->args);
  }
}

static int func_idx(Obj *fn, Token *tok) {
  void *v = hashmap_get(&func_index, fn->name);
  if (!v) error_tok(tok, "function %s is neither defined nor an env import", fn->name);
  return (int)(long)v - 1;
}

static HashMap global_def;  // name -> defining Obj (declarations are separate Objs in chibicc)

static int global_addr(Obj *var, Token *tok) {
  if (var->is_function) return func_idx(var, tok) + 1; // table index
  Obj *def = hashmap_get(&global_def, var->name);
  if (!def) {
    if (!strcmp(var->name, "__heap_base")) return heap_base;
    error_tok(tok, "undefined global %s", var->name);
  }
  return def->offset;
}

// ---- loads/stores ----
static int align_log2(int n) { int r = 0; while ((1 << r) < n && r < 3) r++; return r; }

static void load(Type *ty) {
  switch (ty->kind) {
  case TY_ARRAY: case TY_STRUCT: case TY_UNION: case TY_FUNC: case TY_VLA: return;
  case TY_FLOAT: memarg(OP_F32_LOAD, 2); return;
  case TY_DOUBLE: case TY_LDOUBLE: memarg(OP_F64_LOAD, 3); return;
  default: break;
  }
  switch (ty->size) {
  case 1: memarg(ty->is_unsigned || ty->kind == TY_BOOL ? OP_I32_LOAD8_U : OP_I32_LOAD8_S, 0); return;
  case 2: memarg(ty->is_unsigned ? OP_I32_LOAD16_U : OP_I32_LOAD16_S, 1); return;
  case 4: memarg(OP_I32_LOAD, 2); return;
  case 8: memarg(OP_I64_LOAD, 3); return;
  }
  unreachable();
}

static void store(Type *ty) {
  switch (ty->kind) {
  case TY_FLOAT: memarg(OP_F32_STORE, 2); return;
  case TY_DOUBLE: case TY_LDOUBLE: memarg(OP_F64_STORE, 3); return;
  default: break;
  }
  switch (ty->size) {
  case 1: memarg(OP_I32_STORE8, 0); return;
  case 2: memarg(OP_I32_STORE16, 1); return;
  case 4: memarg(OP_I32_STORE, 2); return;
  case 8: memarg(OP_I64_STORE, 3); return;
  }
  unreachable();
}

// ---- casts ----
static void to_bool(int vc) {
  switch (vc) {
  case VC_I32: i32c(0); op(OP_I32_NE); return;
  case VC_I64: i64c(0); op(OP_I64_NE); return;
  case VC_F32: f32c(0); op(OP_F32_NE); return;
  case VC_F64: f64c(0); op(OP_F64_NE); return;
  }
}

// value of class `from` on the stack -> class `to`
static void convert(int from, int to, bool from_unsigned) {
  if (from == to) return;
  switch (from) {
  case VC_I32:
    switch (to) {
    case VC_I64: op(from_unsigned ? OP_I64_EXTEND_I32_U : OP_I64_EXTEND_I32_S); return;
    case VC_F32: op(from_unsigned ? OP_F32_CONVERT_I32_U : OP_F32_CONVERT_I32_S); return;
    case VC_F64: op(from_unsigned ? OP_F64_CONVERT_I32_U : OP_F64_CONVERT_I32_S); return;
    }
    break;
  case VC_I64:
    switch (to) {
    case VC_I32: op(OP_I32_WRAP_I64); return;
    case VC_F32: op(from_unsigned ? OP_F32_CONVERT_I64_U : OP_F32_CONVERT_I64_S); return;
    case VC_F64: op(from_unsigned ? OP_F64_CONVERT_I64_U : OP_F64_CONVERT_I64_S); return;
    }
    break;
  case VC_F32:
    switch (to) {
    case VC_F64: op(OP_F64_PROMOTE_F32); return;
    case VC_I32: op(from_unsigned ? OP_I32_TRUNC_F32_U : OP_I32_TRUNC_F32_S); return;
    case VC_I64: op(from_unsigned ? OP_I64_TRUNC_F32_U : OP_I64_TRUNC_F32_S); return;
    }
    break;
  case VC_F64:
    switch (to) {
    case VC_F32: op(OP_F32_DEMOTE_F64); return;
    case VC_I32: op(from_unsigned ? OP_I32_TRUNC_F64_U : OP_I32_TRUNC_F64_S); return;
    case VC_I64: op(from_unsigned ? OP_I64_TRUNC_F64_U : OP_I64_TRUNC_F64_S); return;
    }
    break;
  }
  unreachable();
}

// narrow an i32 to the representation of a small integer type
static void narrow(Type *to) {
  if (to->kind == TY_BOOL) return; // handled by caller
  switch (to->size) {
  case 1: if (to->is_unsigned) { i32c(0xff); op(OP_I32_AND); } else op(OP_I32_EXTEND8_S); return;
  case 2: if (to->is_unsigned) { i32c(0xffff); op(OP_I32_AND); } else op(OP_I32_EXTEND16_S); return;
  }
}

static void cast(Type *from, Type *to) {
  int fc = vclass(from), tc = vclass(to);
  if (to->kind == TY_VOID) { if (fc != VC_NONE) op(OP_DROP); return; }
  if (to->kind == TY_BOOL) { to_bool(fc); return; }
  // when converting a float to a small integer, go through i32 with the target's signedness
  bool from_unsigned = from->is_unsigned || from->kind == TY_PTR || from->kind == TY_ARRAY;
  if ((fc == VC_F32 || fc == VC_F64) && (tc == VC_I32 || tc == VC_I64))
    from_unsigned = to->is_unsigned;
  convert(fc, tc, from_unsigned);
  if (tc == VC_I32 && is_integer(to) && to->size < 4)
    if (from->size > to->size || fc != VC_I32 || from->is_unsigned != to->is_unsigned)
      narrow(to);
}

// generate expression and convert its value to class vc
static void gen_expr_as(Node *node, int vc) {
  gen_expr(node);
  convert(vclass(node->ty), vc, node->ty->is_unsigned || node->ty->kind == TY_PTR);
}

// evaluate to an i32 that is 0 or 1
static void gen_cond(Node *node) {
  gen_expr(node);
  to_bool(vclass(node->ty));
}

// ---- addresses ----
static void fp_plus(int off) {
  op_u(OP_LOCAL_GET, loc_fp);
  if (off) { i32c(off); op(OP_I32_ADD); }
}

static void gen_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR:
    if (node->var->ty->kind == TY_VLA) error_tok(node->tok, "VLA is not supported");
    if (node->var->is_local) { fp_plus(node->var->offset); return; }
    if (node->var->is_tls) error_tok(node->tok, "TLS is not supported");
    i32c(global_addr(node->var, node->tok));
    return;
  case ND_DEREF:
    gen_expr_as(node->lhs, VC_I32);
    return;
  case ND_COMMA:
    gen_expr(node->lhs);
    if (vclass(node->lhs->ty) != VC_NONE) op(OP_DROP);
    gen_addr(node->rhs);
    return;
  case ND_MEMBER:
    gen_addr(node->lhs);
    if (node->member->offset) { i32c(node->member->offset); op(OP_I32_ADD); }
    return;
  case ND_FUNCALL:
    if (node->ret_buffer) { gen_expr(node); return; }
    break;
  case ND_ASSIGN:
  case ND_COND:
    if (is_struct(node->ty)) { gen_expr(node); return; }
    break;
  default:
    break;
  }
  error_tok(node->tok, "not an lvalue");
}

// ---- calls ----
static int count_params(Type *fty) { int n = 0; for (Type *t = fty->params; t; t = t->next) n++; return n; }

static void gen_funcall(Node *node) {
  Type *fty = node->func_ty;
  Obj *callee = (node->lhs->kind == ND_VAR && node->lhs->var->is_function) ? node->lhs->var : NULL;

  if (callee) {
    if (!strcmp(callee->name, "__builtin_wasm_memory_size")) {
      for (Node *a = node->args; a; a = a->next) { gen_expr(a); op(OP_DROP); }
      op(OP_MEMORY_SIZE); buf_u8(&code, 0);
      convert(VC_I32, vclass(node->ty), true);
      return;
    }
    if (!strcmp(callee->name, "__builtin_wasm_memory_grow")) {
      gen_expr(node->args); op(OP_DROP);
      gen_expr_as(node->args->next, VC_I32);
      op(OP_MEMORY_GROW); buf_u8(&code, 0);
      convert(VC_I32, vclass(node->ty), true);
      return;
    }
    if (!strcmp(callee->name, "alloca")) error_tok(node->tok, "alloca is not supported");
  }

  if (node->ret_buffer) fp_plus(node->ret_buffer->offset);

  int nfixed = count_params(fty);
  Node *arg = node->args;
  for (int i = 0; i < nfixed; i++, arg = arg->next) gen_expr(arg);

  if (fty->is_variadic) {
    int nva = 0;
    for (Node *a = arg; a; a = a->next) nva++;
    int size = align_to(nva * 8, 16);
    if (size) {
      op_u(OP_GLOBAL_GET, 0); i32c(size); op(OP_I32_SUB); op_u(OP_GLOBAL_SET, 0);
    }
    int slot = 0;
    for (Node *a = arg; a; a = a->next, slot++) {
      if (is_struct(a->ty)) error_tok(a->tok, "passing a struct as a variadic argument is not supported");
      op_u(OP_GLOBAL_GET, 0);
      if (slot) { i32c(slot * 8); op(OP_I32_ADD); }
      gen_expr(a);
      switch (vclass(a->ty)) {
      case VC_I32: memarg(OP_I32_STORE, 2); break;
      case VC_I64: memarg(OP_I64_STORE, 3); break;
      case VC_F32: memarg(OP_F32_STORE, 2); break;
      case VC_F64: memarg(OP_F64_STORE, 3); break;
      default: unreachable();
      }
    }
    op_u(OP_GLOBAL_GET, 0); // the va pointer argument
    if (callee) op_u(OP_CALL, func_idx(callee, node->tok));
    else { gen_expr_as(node->lhs, VC_I32); op(OP_CALL_INDIRECT); buf_uleb(&code, sig_index(fty)); buf_u8(&code, 0); }
    if (size) {
      op_u(OP_GLOBAL_GET, 0); i32c(size); op(OP_I32_ADD); op_u(OP_GLOBAL_SET, 0);
    }
  } else {
    if (arg) error_tok(node->tok, "too many arguments");
    if (callee) op_u(OP_CALL, func_idx(callee, node->tok));
    else { gen_expr_as(node->lhs, VC_I32); op(OP_CALL_INDIRECT); buf_uleb(&code, sig_index(fty)); buf_u8(&code, 0); }
  }

  if (node->ret_buffer) fp_plus(node->ret_buffer->offset);
}

// ---- expressions ----
static int scratch_local(int vc) {
  switch (vc) {
  case VC_I32: return loc_t32;
  case VC_I64: return loc_t64;
  case VC_F32: return loc_tf32;
  case VC_F64: return loc_tf64;
  }
  unreachable();
}

static void gen_binary(Node *node) {
  bool fl = is_flonum(node->lhs->ty);
  int vc = vclass(node->lhs->ty);
  bool u = node->lhs->ty->is_unsigned || node->lhs->ty->kind == TY_PTR || node->lhs->ty->kind == TY_ARRAY;
  bool cmp = node->kind == ND_EQ || node->kind == ND_NE || node->kind == ND_LT || node->kind == ND_LE;
  if (!cmp) vc = vclass(node->ty);

  gen_expr_as(node->lhs, vc);
  gen_expr_as(node->rhs, vc);

  if (fl) {
    bool d = vc == VC_F64;
    switch (node->kind) {
    case ND_ADD: op(d ? OP_F64_ADD : OP_F32_ADD); return;
    case ND_SUB: op(d ? OP_F64_SUB : OP_F32_SUB); return;
    case ND_MUL: op(d ? OP_F64_MUL : OP_F32_MUL); return;
    case ND_DIV: op(d ? OP_F64_DIV : OP_F32_DIV); return;
    case ND_EQ: op(d ? OP_F64_EQ : OP_F32_EQ); return;
    case ND_NE: op(d ? OP_F64_NE : OP_F32_NE); return;
    case ND_LT: op(d ? OP_F64_LT : OP_F32_LT); return;
    case ND_LE: op(d ? OP_F64_LE : OP_F32_LE); return;
    default: error_tok(node->tok, "invalid float operation");
    }
  }
  bool w = vc == VC_I64;
  switch (node->kind) {
  case ND_ADD: op(w ? OP_I64_ADD : OP_I32_ADD); return;
  case ND_SUB: op(w ? OP_I64_SUB : OP_I32_SUB); return;
  case ND_MUL: op(w ? OP_I64_MUL : OP_I32_MUL); return;
  case ND_DIV: op(w ? (u ? OP_I64_DIV_U : OP_I64_DIV_S) : (u ? OP_I32_DIV_U : OP_I32_DIV_S)); return;
  case ND_MOD: op(w ? (u ? OP_I64_REM_U : OP_I64_REM_S) : (u ? OP_I32_REM_U : OP_I32_REM_S)); return;
  case ND_BITAND: op(w ? OP_I64_AND : OP_I32_AND); return;
  case ND_BITOR: op(w ? OP_I64_OR : OP_I32_OR); return;
  case ND_BITXOR: op(w ? OP_I64_XOR : OP_I32_XOR); return;
  case ND_SHL: op(w ? OP_I64_SHL : OP_I32_SHL); return;
  case ND_SHR: op(w ? (u ? OP_I64_SHR_U : OP_I64_SHR_S) : (u ? OP_I32_SHR_U : OP_I32_SHR_S)); return;
  case ND_EQ: op(w ? OP_I64_EQ : OP_I32_EQ); return;
  case ND_NE: op(w ? OP_I64_NE : OP_I32_NE); return;
  case ND_LT: op(w ? (u ? OP_I64_LT_U : OP_I64_LT_S) : (u ? OP_I32_LT_U : OP_I32_LT_S)); return;
  case ND_LE: op(w ? (u ? OP_I64_LE_U : OP_I64_LE_S) : (u ? OP_I32_LE_U : OP_I32_LE_S)); return;
  default: unreachable();
  }
}

static void gen_expr(Node *node) {
  switch (node->kind) {
  case ND_NULL_EXPR:
    return;
  case ND_NUM:
    switch (vclass(node->ty)) {
    case VC_F32: f32c((float)node->fval); return;
    case VC_F64: f64c((double)node->fval); return;
    case VC_I64: i64c(node->val); return;
    default: i32c(node->val); return;
    }
  case ND_NEG:
    switch (vclass(node->ty)) {
    case VC_F32: gen_expr(node->lhs); op(OP_F32_NEG); return;
    case VC_F64: gen_expr(node->lhs); op(OP_F64_NEG); return;
    case VC_I64: i64c(0); gen_expr_as(node->lhs, VC_I64); op(OP_I64_SUB); return;
    default: i32c(0); gen_expr_as(node->lhs, VC_I32); op(OP_I32_SUB); return;
    }
  case ND_VAR:
    gen_addr(node);
    load(node->ty);
    return;
  case ND_MEMBER:
    if (node->member->is_bitfield) error_tok(node->tok, "bitfields are not supported");
    gen_addr(node);
    load(node->ty);
    return;
  case ND_DEREF:
    gen_addr(node);
    load(node->ty);
    return;
  case ND_ADDR:
    gen_addr(node->lhs);
    return;
  case ND_ASSIGN: {
    if (node->lhs->kind == ND_MEMBER && node->lhs->member->is_bitfield)
      error_tok(node->tok, "bitfields are not supported");
    if (is_struct(node->ty)) {
      gen_addr(node->lhs);
      op_u(OP_LOCAL_TEE, loc_t32b);
      gen_expr(node->rhs);
      i32c(node->ty->size);
      memory_copy();
      op_u(OP_LOCAL_GET, loc_t32b);
      return;
    }
    int vc = vclass(node->ty);
    gen_addr(node->lhs);
    gen_expr_as(node->rhs, vc);
    op_u(OP_LOCAL_TEE, scratch_local(vc));
    store(node->ty);
    op_u(OP_LOCAL_GET, scratch_local(vc));
    return;
  }
  case ND_STMT_EXPR:
    error_tok(node->tok, "statement expressions are not supported");
  case ND_COMMA:
    gen_expr(node->lhs);
    if (vclass(node->lhs->ty) != VC_NONE) op(OP_DROP);
    gen_expr(node->rhs);
    return;
  case ND_CAST:
    gen_expr(node->lhs);
    cast(node->lhs->ty, node->ty);
    return;
  case ND_MEMZERO:
    fp_plus(node->var->offset);
    i32c(0);
    i32c(node->var->ty->size);
    memory_fill();
    return;
  case ND_COND: {
    int vc = vclass(node->ty);
    gen_cond(node->cond);
    if_(vc == VC_NONE ? 0x40 : valtype[vc]);
    gen_expr_as(node->then, vc);
    op(OP_ELSE);
    gen_expr_as(node->els, vc);
    end();
    return;
  }
  case ND_NOT:
    gen_cond(node->lhs);
    op(OP_I32_EQZ);
    return;
  case ND_BITNOT:
    if (vclass(node->ty) == VC_I64) { gen_expr_as(node->lhs, VC_I64); i64c(-1); op(OP_I64_XOR); }
    else { gen_expr_as(node->lhs, VC_I32); i32c(-1); op(OP_I32_XOR); }
    return;
  case ND_LOGAND:
    gen_cond(node->lhs);
    if_(0x7f);
    gen_cond(node->rhs);
    op(OP_ELSE);
    i32c(0);
    end();
    return;
  case ND_LOGOR:
    gen_cond(node->lhs);
    if_(0x7f);
    i32c(1);
    op(OP_ELSE);
    gen_cond(node->rhs);
    end();
    return;
  case ND_FUNCALL:
    gen_funcall(node);
    return;
  case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
  case ND_BITAND: case ND_BITOR: case ND_BITXOR: case ND_SHL: case ND_SHR:
  case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
    gen_binary(node);
    return;
  case ND_LABEL_VAL: case ND_CAS: case ND_EXCH: case ND_VLA_PTR:
    error_tok(node->tok, "unsupported expression");
  default:
    error_tok(node->tok, "invalid expression");
  }
}

// ---- statements ----
static void collect_cases(Node *n, Node **out, int *count, int max, Token *tok) {
  // cases at the top level of the switch body, in source order; a case's
  // lhs may itself be a case (case 1: case 2: stmt)
  for (; n; n = n->next)
    for (Node *c = n; c->kind == ND_CASE; c = c->lhs) {
      if (*count == max) error_tok(tok, "too many cases");
      out[(*count)++] = c;
    }
}

static void gen_switch(Node *node) {
  int vc = vclass(node->cond->ty);
  gen_expr(node->cond);
  op_u(OP_LOCAL_SET, scratch_local(vc));

  Node *cases[1024];
  int ncases = 0;
  Node *body = node->then;
  if (body->kind == ND_BLOCK) collect_cases(body->body, cases, &ncases, 1024, node->tok);
  else collect_cases(body, cases, &ncases, 1024, node->tok);

  block(node->brk_label, 0x40);
  for (int i = ncases - 1; i >= 0; i--) block(cases[i]->label, 0x40);

  for (Node *c = node->case_next; c; c = c->case_next) {
    op_u(OP_LOCAL_GET, scratch_local(vc));
    if (c->begin == c->end) {
      if (vc == VC_I64) { i64c(c->begin); op(OP_I64_EQ); } else { i32c(c->begin); op(OP_I32_EQ); }
    } else {
      if (vc == VC_I64) { i64c(c->begin); op(OP_I64_SUB); i64c(c->end - c->begin); op(OP_I64_LE_U); }
      else { i32c(c->begin); op(OP_I32_SUB); i32c(c->end - c->begin); op(OP_I32_LE_U); }
    }
    br_if_label(c->label, c->tok);
  }
  br_label(node->default_case ? node->default_case->label : node->brk_label, node->tok);

  gen_stmt(node->then);
  // every case block must have been closed by its ND_CASE
  if (ctl_len == 0 || !ctl[ctl_len - 1].label || strcmp(ctl[ctl_len - 1].label, node->brk_label))
    error_tok(node->tok, "unsupported switch shape (a case is nested inside another statement)");
  end();
}

static void gen_stmt(Node *node) {
  switch (node->kind) {
  case ND_BLOCK:
    for (Node *n = node->body; n; n = n->next) gen_stmt(n);
    return;
  case ND_EXPR_STMT:
    gen_expr(node->lhs);
    if (vclass(node->lhs->ty) != VC_NONE) op(OP_DROP);
    return;
  case ND_IF:
    gen_cond(node->cond);
    if_(0x40);
    gen_stmt(node->then);
    if (node->els) { op(OP_ELSE); gen_stmt(node->els); }
    end();
    return;
  case ND_FOR: {
    if (node->init) gen_stmt(node->init);
    block(node->brk_label, 0x40);
    int top = loop();
    if (node->cond) { gen_cond(node->cond); op(OP_I32_EQZ); br_if_label(node->brk_label, node->tok); }
    block(node->cont_label, 0x40);
    gen_stmt(node->then);
    end();
    if (node->inc) { gen_expr(node->inc); if (vclass(node->inc->ty) != VC_NONE) op(OP_DROP); }
    op_u(OP_BR, depth_of_id(top));
    end();
    end();
    return;
  }
  case ND_DO: {
    block(node->brk_label, 0x40);
    int top = loop();
    block(node->cont_label, 0x40);
    gen_stmt(node->then);
    end();
    gen_cond(node->cond);
    op_u(OP_BR_IF, depth_of_id(top));
    end();
    end();
    return;
  }
  case ND_SWITCH:
    gen_switch(node);
    return;
  case ND_CASE:
    if (ctl_len == 0 || !ctl[ctl_len - 1].label || strcmp(ctl[ctl_len - 1].label, node->label))
      error_tok(node->tok, "unsupported switch shape (case nested inside another statement)");
    end();
    gen_stmt(node->lhs);
    return;
  case ND_GOTO:
    br_label(node->unique_label, node->tok);
    return;
  case ND_LABEL:
    if (ctl_len == 0 || !ctl[ctl_len - 1].label || strcmp(ctl[ctl_len - 1].label, node->unique_label))
      error_tok(node->tok, "unsupported label position (labels must be at the top level of the function body)");
    end();
    gen_stmt(node->lhs);
    return;
  case ND_RETURN:
    if (node->lhs) {
      Type *ty = node->lhs->ty;
      if (is_struct(ty)) {
        op_u(OP_LOCAL_GET, 0); // hidden return pointer
        gen_expr(node->lhs);
        i32c(ty->size);
        memory_copy();
      } else {
        gen_expr_as(node->lhs, vclass(current_fn->ty->return_ty));
        op_u(OP_LOCAL_SET, loc_rv);
      }
    }
    br_label(RET_LABEL, node->tok);
    return;
  case ND_GOTO_EXPR: case ND_ASM:
    error_tok(node->tok, "unsupported statement");
  default:
    error_tok(node->tok, "invalid statement");
  }
}

// ---- functions ----
static void assign_lvar_offsets(Obj *fn) {
  int off = 0;
  for (Obj *var = fn->locals; var; var = var->next) {
    if (var->ty->kind == TY_VLA) error_tok(var->tok, "VLA is not supported");
    int align = var->align < 1 ? 1 : var->align;
    off = align_to(off, align);
    var->offset = off;
    off += var->ty->size;
  }
  frame_size = align_to(off, 16);
}

static void gen_function(Obj *fn, Buf *out) {
  current_fn = fn;
  code.len = 0;
  ctl_len = 0;
  assign_lvar_offsets(fn);

  int nparams = 0;
  for (Obj *p = fn->params; p; p = p->next) nparams++;
  if (fn->ty->is_variadic) nparams++;
  loc_fp = nparams; loc_t32 = nparams + 1; loc_t32b = nparams + 2;
  loc_t64 = nparams + 3; loc_tf32 = nparams + 4; loc_tf64 = nparams + 5; loc_rv = nparams + 6;
  int rc = is_struct(fn->ty->return_ty) ? VC_NONE : vclass(fn->ty->return_ty);

  // prologue: fp = sp - frame; sp = fp
  op_u(OP_GLOBAL_GET, 0);
  if (frame_size) { i32c(frame_size); op(OP_I32_SUB); }
  op_u(OP_LOCAL_TEE, loc_fp);
  op_u(OP_GLOBAL_SET, 0);

  // spill parameters into their frame slots
  int i = 0;
  for (Obj *p = fn->params; p; p = p->next, i++) {
    if (is_struct(p->ty)) {
      fp_plus(p->offset); op_u(OP_LOCAL_GET, i); i32c(p->ty->size); memory_copy();
    } else {
      fp_plus(p->offset); op_u(OP_LOCAL_GET, i); store(p->ty);
    }
  }
  if (fn->ty->is_variadic) {
    fp_plus(fn->va_area->offset); op_u(OP_LOCAL_GET, i); memarg(OP_I32_STORE, 2);
  }

  // blocks for forward gotos to top-level labels
  Node *body = fn->body;
  Node *labels[256];
  int nlabels = 0;
  if (body->kind == ND_BLOCK)
    for (Node *n = body->body; n; n = n->next)
      if (n->kind == ND_LABEL) {
        if (nlabels == 256) error_tok(n->tok, "too many labels");
        labels[nlabels++] = n;
      }
  block(RET_LABEL, 0x40);
  for (int k = nlabels - 1; k >= 0; k--) block(labels[k]->unique_label, 0x40);

  gen_stmt(body);

  // unclosed label blocks (label never reached at top level) would be a bug
  if (ctl_len != 1) error_tok(fn->tok, "internal error: unbalanced control stack in %s", fn->name);
  end();

  // epilogue: sp = fp + frame
  op_u(OP_LOCAL_GET, loc_fp);
  if (frame_size) { i32c(frame_size); op(OP_I32_ADD); }
  op_u(OP_GLOBAL_SET, 0);
  if (rc != VC_NONE) op_u(OP_LOCAL_GET, loc_rv);
  op(OP_END);

  // function body: locals declaration + code
  Buf body_buf = {0};
  buf_uleb(&body_buf, rc == VC_NONE ? 4 : 5);
  buf_uleb(&body_buf, 3); buf_u8(&body_buf, 0x7f);
  buf_uleb(&body_buf, 1); buf_u8(&body_buf, 0x7e);
  buf_uleb(&body_buf, 1); buf_u8(&body_buf, 0x7d);
  buf_uleb(&body_buf, 1); buf_u8(&body_buf, 0x7c);
  if (rc != VC_NONE) { buf_uleb(&body_buf, 1); buf_u8(&body_buf, valtype[rc]); }
  buf_buf(&body_buf, &code);
  buf_uleb(out, body_buf.len);
  buf_buf(out, &body_buf);
  free(body_buf.data);
}

// ---- data ----
static void layout_globals(Obj *prog) {
  int64_t addr = data_base;
  // initialised globals first, then zero-initialised ones, so one data segment covers the former;
  // one storage location per name (a tentative definition repeated in several headers is one object)
  for (int pass = 0; pass < 2; pass++) {
    for (Obj *var = prog; var; var = var->next) {
      if (var->is_function || !var->is_definition) continue;
      if ((var->init_data != NULL) != (pass == 0)) continue;
      if (hashmap_get(&global_def, var->name)) {
        if (var->init_data) error_tok(var->tok, "redefinition of %s", var->name);
        continue;
      }
      hashmap_put(&global_def, var->name, var);
      int align = var->align < 1 ? 1 : var->align;
      addr = align_to((int)addr, align);
      var->offset = (int)addr;
      addr += var->ty->size;
      if (addr > 0x7fffffff) error("globals do not fit in 2 GiB");
    }
    if (pass == 0) data_end = (int)addr;
  }
  int64_t stack_bottom = align_to(addr, 16);
  int64_t top = stack_bottom + align_to(opt_stack_size, 16);
  if (top + 65535 > (int64_t)opt_max_pages * 65536 || top > 0x7fffffff)
    error("data (%d bytes) plus stack (%d bytes) do not fit in %d pages", (int)addr, opt_stack_size, opt_max_pages);
  stack_top = (int)top;
  heap_base = stack_top;
}

static void emit_data(Obj *prog, Buf *sec) {
  int n = data_end - data_base;
  uint8_t *img = calloc(1, n ? n : 1);
  for (Obj *var = prog; var; var = var->next) {
    if (var->is_function || !var->is_definition || !var->init_data) continue;
    if (hashmap_get(&global_def, var->name) != var) continue;
    memcpy(img + var->offset - data_base, var->init_data, var->ty->size);
    for (Relocation *rel = var->rel; rel; rel = rel->next) {
      Obj *target = NULL;
      for (Obj *o = prog; o; o = o->next)
        if (!strcmp(o->name, *rel->label)) { target = o; break; }
      if (!target) error("relocation to unknown symbol %s", *rel->label);
      uint32_t v = (uint32_t)(global_addr(target, var->tok) + rel->addend);
      for (int k = 0; k < 4; k++) img[var->offset - data_base + rel->offset + k] = (v >> (8 * k)) & 0xff;
    }
  }
  buf_uleb(sec, 1);               // one segment
  buf_u8(sec, 0);                 // active, memory 0
  buf_u8(sec, OP_I32_CONST); buf_sleb(sec, data_base); buf_u8(sec, OP_END);
  buf_uleb(sec, n);
  buf_bytes(sec, img, n);
  free(img);
}

int align_to(int n, int align) { return (n + align - 1) / align * align; }

void codegen(Obj *prog, FILE *out) {
  // the parser prepends, so prog is in reverse source order; reverse it for stable numbering
  Obj *rev = NULL;
  for (Obj *o = prog; o;) { Obj *next = o->next; o->next = rev; rev = o; o = next; }
  prog = rev;

  for (Obj *fn = prog; fn; fn = fn->next)
    if (fn->is_function && fn->is_definition) mark_refs(fn->body);
  for (Obj *var = prog; var; var = var->next)
    if (!var->is_function && var->is_definition)
      for (Relocation *rel = var->rel; rel; rel = rel->next)
        hashmap_put(&referenced, *rel->label, (void *)1);

  // number functions: env imports first, then definitions
  Buf imports = {0}, funcsec = {0};
  int cap = 64;
  funcs = calloc(cap, sizeof(Obj *));
  for (int pass = 0; pass < 2; pass++) {
    for (Obj *fn = prog; fn; fn = fn->next) {
      if (!fn->is_function) continue;
      if (pass == 0) {
        if (fn->is_definition || !hashmap_get(&referenced, fn->name)) continue;
        if (!strncmp(fn->name, "__builtin_", 10)) continue; // handled inline by gen_funcall
        if (strncmp(fn->name, "__env_", 6)) error_tok(fn->tok, "undefined function %s (env imports must be named __env_*)", fn->name);
        buf_name(&imports, "env"); buf_name(&imports, fn->name + 6);
        buf_u8(&imports, 0); buf_uleb(&imports, sig_index(fn->ty));
      } else {
        if (!fn->is_definition) continue;
        buf_uleb(&funcsec, sig_index(fn->ty));
      }
      if (nfuncs == cap) { cap *= 2; funcs = realloc(funcs, cap * sizeof(Obj *)); }
      funcs[nfuncs] = fn;
      hashmap_put(&func_index, fn->name, (void *)(long)(nfuncs + 1));
      nfuncs++;
    }
    if (pass == 0) nimports = nfuncs;
  }

  layout_globals(prog);

  Buf codesec = {0};
  buf_uleb(&codesec, nfuncs - nimports);
  for (int i = nimports; i < nfuncs; i++) gen_function(funcs[i], &codesec);

  Buf mod = {0};
  buf_bytes(&mod, "\0asm\1\0\0\0", 8);

  Buf s = {0};
  buf_uleb(&s, ntypes); buf_buf(&s, &types);
  buf_section(&mod, 1, &s);

  if (nimports) {
    s.len = 0; buf_uleb(&s, nimports); buf_buf(&s, &imports);
    buf_section(&mod, 2, &s);
  }

  s.len = 0; buf_uleb(&s, nfuncs - nimports); buf_buf(&s, &funcsec);
  buf_section(&mod, 3, &s);

  s.len = 0; buf_uleb(&s, 1); buf_u8(&s, 0x70); buf_u8(&s, 1); buf_uleb(&s, nfuncs + 1); buf_uleb(&s, nfuncs + 1);
  buf_section(&mod, 4, &s);

  int min_pages = (heap_base + 65535) / 65536;
  if (min_pages > opt_max_pages) error("initial memory (%d pages) exceeds -mmaxpages=%d", min_pages, opt_max_pages);
  s.len = 0; buf_uleb(&s, 1); buf_u8(&s, 1); buf_uleb(&s, min_pages); buf_uleb(&s, opt_max_pages);
  buf_section(&mod, 5, &s);

  s.len = 0; buf_uleb(&s, 1); buf_u8(&s, 0x7f); buf_u8(&s, 1);
  buf_u8(&s, OP_I32_CONST); buf_sleb(&s, stack_top); buf_u8(&s, OP_END);
  buf_section(&mod, 6, &s);

  s.len = 0;
  int nexports = 1;
  void *start = hashmap_get(&func_index, "_start");
  if (start) nexports++;
  buf_uleb(&s, nexports);
  buf_name(&s, "memory"); buf_u8(&s, 2); buf_uleb(&s, 0);
  if (start) { buf_name(&s, "_start"); buf_u8(&s, 0); buf_uleb(&s, (int)(long)start - 1); }
  buf_section(&mod, 7, &s);

  s.len = 0; buf_uleb(&s, 1); buf_u8(&s, 0);
  buf_u8(&s, OP_I32_CONST); buf_sleb(&s, 1); buf_u8(&s, OP_END);
  buf_uleb(&s, nfuncs);
  for (int i = 0; i < nfuncs; i++) buf_uleb(&s, i);
  buf_section(&mod, 9, &s);

  buf_section(&mod, 10, &codesec);

  s.len = 0; emit_data(prog, &s);
  buf_section(&mod, 11, &s);

  fwrite(mod.data, 1, mod.len, out);
}
