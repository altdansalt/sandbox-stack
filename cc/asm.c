// Assembler and static ELF64 writer for the subset of x86-64 GNU-syntax
// assembly that chibicc's x86-64 codegen emits. Everything is one object:
// symbols resolve within the image, so there are no relocations in the
// output. Unknown instructions are errors, never silently skipped.
//
// Supported: integer moves/ALU/shift/setcc/jumps/call/ret, movzx/movsx
// family, lea, push/pop, imul/idiv/div/mul/neg/not, cqo/cdq, rep stosb,
// scalar SSE (movss/movsd/movq/cvt*/ucomis*/add/sub/mul/div/xorps/pxor),
// syscall/hlt, and the directives .text .data .bss .align .byte .quad
// .zero .comm .globl .local .type .size .file .loc .section(.text/.data).
// Not supported: x87 (long double), TLS, atomics.
#include "chibicc.h"

typedef struct { uint8_t *data; int len, cap; } Sec;
static Sec text, data;
static long bss_len;
static Sec *cur;               // current section, or NULL for .bss
static bool cur_bss;

typedef struct { int sec; long off; } Sym;  // sec: 0 text, 1 data, 2 bss
static HashMap syms;

typedef struct { int sec; long off; char *name; long addend; bool abs64; } Fixup;
static Fixup *fixups;
static int nfixups, fixcap;

static int line_no;
static char *cur_line;

static noreturn void asm_error(char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "asm: line %d: ", line_no);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n  %s\n", cur_line);
  exit(1);
}

static void sec_u8(Sec *s, int v) {
  if (s->len == s->cap) { s->cap = s->cap ? s->cap * 2 : 1 << 16; s->data = realloc(s->data, s->cap); }
  s->data[s->len++] = (uint8_t)v;
}
static void emit8(int v) { sec_u8(cur, v); }
static void emit32(uint32_t v) { for (int i = 0; i < 4; i++) emit8((v >> (8 * i)) & 0xff); }
static void emit64(uint64_t v) { for (int i = 0; i < 8; i++) emit8((v >> (8 * i)) & 0xff); }

static void add_fixup(char *name, long addend, bool abs64) {
  if (nfixups == fixcap) { fixcap = fixcap ? fixcap * 2 : 1024; fixups = realloc(fixups, fixcap * sizeof(Fixup)); }
  fixups[nfixups++] = (Fixup){ cur == &text ? 0 : 1, cur->len, name, addend, abs64 };
}

static void define_sym(char *name) {
  Sym *s = calloc(1, sizeof(Sym));
  s->sec = cur_bss ? 2 : (cur == &text ? 0 : 1);
  s->off = cur_bss ? bss_len : cur->len;
  if (hashmap_get(&syms, name)) asm_error("duplicate symbol %s", name);
  hashmap_put(&syms, name, s);
}

// numeric local labels: "1:" defines, "1f" refers to the next definition
static int numeric_count[10];
static char *numeric_name(int n, int k) { return format(".num%d.%d", n, k); }

// ---- operands ----
enum { O_NONE, O_REG, O_IMM, O_MEM, O_LABEL };
typedef struct {
  int kind;
  int reg, size;        // O_REG: register number 0-15, size 1/2/4/8, 16 for xmm; high8 for ah..bh
  bool high8, xmm;
  int64_t imm;          // O_IMM
  int base;             // O_MEM: base register, -1 = rip
  int64_t disp;
  char *sym;            // O_MEM rip-relative symbol, or O_LABEL name
  bool gotpcrel;        // sym@GOTPCREL: the value is the symbol's address (static image: encode as lea)
} Op;

static struct { char *name; int reg, size; bool high8, xmm; } regs[] = {
  {"rax",0,8},{"rcx",1,8},{"rdx",2,8},{"rbx",3,8},{"rsp",4,8},{"rbp",5,8},{"rsi",6,8},{"rdi",7,8},
  {"r8",8,8},{"r9",9,8},{"r10",10,8},{"r11",11,8},{"r12",12,8},{"r13",13,8},{"r14",14,8},{"r15",15,8},
  {"eax",0,4},{"ecx",1,4},{"edx",2,4},{"ebx",3,4},{"esp",4,4},{"ebp",5,4},{"esi",6,4},{"edi",7,4},
  {"r8d",8,4},{"r9d",9,4},{"r10d",10,4},{"r11d",11,4},{"r12d",12,4},{"r13d",13,4},{"r14d",14,4},{"r15d",15,4},
  {"ax",0,2},{"cx",1,2},{"dx",2,2},{"bx",3,2},{"sp",4,2},{"bp",5,2},{"si",6,2},{"di",7,2},
  {"r8w",8,2},{"r9w",9,2},{"r10w",10,2},{"r11w",11,2},{"r12w",12,2},{"r13w",13,2},{"r14w",14,2},{"r15w",15,2},
  {"al",0,1},{"cl",1,1},{"dl",2,1},{"bl",3,1},{"spl",4,1},{"bpl",5,1},{"sil",6,1},{"dil",7,1},
  {"r8b",8,1},{"r9b",9,1},{"r10b",10,1},{"r11b",11,1},{"r12b",12,1},{"r13b",13,1},{"r14b",14,1},{"r15b",15,1},
  {"ah",4,1,true},{"ch",5,1,true},{"dh",6,1,true},{"bh",7,1,true},
  {"xmm0",0,16,false,true},{"xmm1",1,16,false,true},{"xmm2",2,16,false,true},{"xmm3",3,16,false,true},
  {"xmm4",4,16,false,true},{"xmm5",5,16,false,true},{"xmm6",6,16,false,true},{"xmm7",7,16,false,true},
  {"rip",-1,8},
};

static bool parse_reg(char *s, Op *o) {
  if (*s != '%') return false;
  for (int i = 0; i < sizeof(regs) / sizeof(*regs); i++)
    if (!strcmp(s + 1, regs[i].name)) {
      o->kind = O_REG; o->reg = regs[i].reg; o->size = regs[i].size; o->high8 = regs[i].high8; o->xmm = regs[i].xmm;
      return true;
    }
  asm_error("unknown register %s", s);
}

static char *trim(char *s) {
  while (isspace(*s)) s++;
  char *e = s + strlen(s);
  while (e > s && isspace(e[-1])) *--e = 0;
  return s;
}

static bool is_symchar(int c) { return isalnum(c) || c == '_' || c == '.' || c == '$'; }

static Op parse_op(char *s) {
  Op o = {0};
  s = trim(s);
  if (*s == '%') { parse_reg(s, &o); return o; }
  if (*s == '$') {
    o.kind = O_IMM;
    char *end;
    errno = 0;
    if (s[1] == '-') o.imm = strtoll(s + 1, &end, 0);
    else o.imm = (int64_t)strtoull(s + 1, &end, 0);
    if (*end || errno) asm_error("bad immediate %s", s);
    return o;
  }
  char *paren = strchr(s, '(');
  if (paren) {
    o.kind = O_MEM;
    char *close = strchr(paren, ')');
    if (!close || close[1]) asm_error("bad memory operand %s", s);
    *close = 0;
    Op b = {0};
    if (strchr(paren + 1, ',')) asm_error("index registers are not supported: %s", s);
    parse_reg(paren + 1, &b);
    o.base = b.reg;
    *paren = 0;
    s = trim(s);
    if (*s == 0) { o.disp = 0; }
    else if (*s == '-' || isdigit(*s)) { char *end; o.disp = strtoll(s, &end, 10); if (*end) asm_error("bad displacement %s", s); }
    else {
      if (o.base != -1) asm_error("symbolic displacement needs %%rip");
      char *at = strstr(s, "@GOTPCREL");
      if (at) { *at = 0; o.gotpcrel = true; }  // static image: the GOT slot would hold the symbol's address
      o.sym = strdup(s);
    }
    return o;
  }
  if (isdigit(*s) && s[1] == 'f' && !s[2]) { o.kind = O_LABEL; o.sym = numeric_name(*s - '0', numeric_count[*s - '0'] + 1); return o; }
  if (*s == '*') { if (s[1] != '%') asm_error("indirect operand must be a register: %s", s); parse_reg(s + 1, &o); return o; }
  o.kind = O_LABEL; o.sym = strdup(s);
  return o;
}

// ---- encoding ----
typedef struct {
  int prefix;     // 0, 0x66, 0xF2, 0xF3 (mandatory prefix, before REX)
  bool w;         // REX.W
  bool need_rex;  // force REX (sil/dil/spl/bpl)
  uint8_t op[3]; int oplen;
  int reg;        // ModRM.reg field (register number or /digit)
  Op *rm;         // O_REG or O_MEM, or NULL for no ModRM
  int imm_size;   // 0,1,2,4,8
  int64_t imm;
  bool opsize16;  // 0x66 operand-size prefix
} Enc;

static void encode(Enc *e) {
  if (e->opsize16) emit8(0x66);
  if (e->prefix) emit8(e->prefix);
  int rex = 0x40;
  if (e->w) rex |= 8;
  if (e->reg >= 8) rex |= 4;
  bool has_rm = e->rm != NULL;
  if (has_rm) {
    if (e->rm->kind == O_REG && e->rm->reg >= 8) rex |= 1;
    if (e->rm->kind == O_MEM && e->rm->base >= 8) rex |= 1;
    if (e->rm->kind == O_REG && e->rm->high8 && rex != 0x40) asm_error("high byte register with REX");
  }
  if (rex != 0x40 || e->need_rex) emit8(rex);
  for (int i = 0; i < e->oplen; i++) emit8(e->op[i]);
  if (has_rm) {
    int r = e->reg & 7;
    if (e->rm->kind == O_REG) {
      emit8(0xC0 | (r << 3) | (e->rm->reg & 7));
    } else if (e->rm->base == -1) {
      emit8((r << 3) | 5);
      add_fixup(e->rm->sym, e->rm->disp - 4 - e->imm_size, false);
      emit32(0);
    } else {
      int b = e->rm->base & 7;
      int64_t d = e->rm->disp;
      int mod = (d == 0 && b != 5) ? 0 : (d >= -128 && d <= 127) ? 1 : 2;
      emit8((mod << 6) | (r << 3) | b);
      if (b == 4) emit8(0x24); // SIB for rsp/r12 base
      if (mod == 1) emit8((int)d & 0xff);
      else if (mod == 2) { if (d < INT32_MIN || d > INT32_MAX) asm_error("displacement out of range"); emit32((uint32_t)d); }
    }
  }
  switch (e->imm_size) {
  case 0: break;
  case 1: emit8((int)e->imm & 0xff); break;
  case 2: emit8(e->imm & 0xff); emit8((e->imm >> 8) & 0xff); break;
  case 4: emit32((uint32_t)e->imm); break;
  case 8: emit64((uint64_t)e->imm); break;
  }
}

static bool fits8(int64_t v) { return v >= -128 && v <= 127; }
static bool fits32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

static int op_size(Op *a, Op *b, char suffix) {
  if (a && a->kind == O_REG && !a->xmm) return a->size;
  if (b && b->kind == O_REG && !b->xmm) return b->size;
  switch (suffix) { case 'b': return 1; case 'w': return 2; case 'l': return 4; case 'q': return 8; }
  asm_error("cannot determine operand size");
}

static void set_size(Enc *e, int size, uint8_t op8, uint8_t op) {
  e->op[0] = size == 1 ? op8 : op; e->oplen = 1;
  e->w = size == 8; e->opsize16 = size == 2;
}

static void need_rex_for(Enc *e, Op *r) {
  if (r && r->kind == O_REG && r->size == 1 && !r->high8 && r->reg >= 4 && r->reg <= 7) e->need_rex = true;
}

// group-1 ALU: add=0 or=1 and=4 sub=5 xor=6 cmp=7
static void alu(int n, Op *src, Op *dst, char suffix) {
  Enc e = {0};
  int size = op_size(dst, src, suffix);
  if (src->kind == O_IMM) {
    if (dst->kind != O_REG && dst->kind != O_MEM) asm_error("bad ALU destination");
    e.rm = dst; e.reg = n;
    if (size == 1) { e.op[0] = 0x80; e.imm_size = 1; }
    else if (fits8(src->imm)) { e.op[0] = 0x83; e.imm_size = 1; }
    else { e.op[0] = 0x81; e.imm_size = size == 2 ? 2 : 4; if (!fits32(src->imm)) asm_error("immediate too large"); }
    e.oplen = 1; e.w = size == 8; e.opsize16 = size == 2; e.imm = src->imm;
    need_rex_for(&e, dst);
    encode(&e);
    return;
  }
  if (src->kind == O_REG && (dst->kind == O_REG || dst->kind == O_MEM)) {  // op r, r/m  (r is source)
    set_size(&e, size, n * 8, n * 8 + 1); e.reg = src->reg; e.rm = dst;
    need_rex_for(&e, src); need_rex_for(&e, dst);
    encode(&e);
    return;
  }
  if (src->kind == O_MEM && dst->kind == O_REG) {  // op r/m, r
    set_size(&e, size, n * 8 + 2, n * 8 + 3); e.reg = dst->reg; e.rm = src;
    need_rex_for(&e, dst);
    encode(&e);
    return;
  }
  asm_error("unsupported ALU operands");
}

static void mov(Op *src, Op *dst, char suffix) {
  Enc e = {0};
  if ((src->kind == O_REG && src->xmm) || (dst->kind == O_REG && dst->xmm)) asm_error("use movq/movss/movsd for xmm");
  int size = op_size(dst, src, suffix);
  if (src->kind == O_IMM) {
    if (dst->kind == O_REG) {
      if (size == 8 && !fits32(src->imm)) { e.w = true; e.op[0] = 0xB8 + (dst->reg & 7); e.oplen = 1; e.imm_size = 8; e.imm = src->imm; if (dst->reg >= 8) { e.rm = NULL; e.reg = 0; /* REX.B via manual */ }
        // manual REX for the B8+r form
        emit8(0x48 | (dst->reg >= 8 ? 1 : 0)); emit8(0xB8 + (dst->reg & 7)); emit64((uint64_t)src->imm); return; }
      if (size == 8) { e.w = true; e.op[0] = 0xC7; e.oplen = 1; e.reg = 0; e.rm = dst; e.imm_size = 4; e.imm = src->imm; encode(&e); return; }
      // B0+r / B8+r
      if (size == 1) { if (!fits8(src->imm) && src->imm > 255) asm_error("immediate too large"); }
      if (size == 2) e.opsize16 = true;
      int rex = 0x40 | (dst->reg >= 8 ? 1 : 0);
      if (rex != 0x40 || (size == 1 && dst->reg >= 4 && dst->reg <= 7 && !dst->high8)) emit8(rex);
      if (e.opsize16) emit8(0x66);
      emit8((size == 1 ? 0xB0 : 0xB8) + (dst->reg & 7));
      if (size == 1) emit8(src->imm & 0xff); else if (size == 2) { emit8(src->imm & 0xff); emit8((src->imm >> 8) & 0xff); } else emit32((uint32_t)src->imm);
      return;
    }
    if (dst->kind == O_MEM) {
      set_size(&e, size, 0xC6, 0xC7); e.reg = 0; e.rm = dst; e.imm_size = size == 8 ? 4 : size == 2 ? 2 : size; e.imm = src->imm;
      if (size == 8 && !fits32(src->imm)) asm_error("immediate too large for memory store");
      encode(&e); return;
    }
  }
  if (src->kind == O_REG && (dst->kind == O_REG || dst->kind == O_MEM)) {
    set_size(&e, size, 0x88, 0x89); e.reg = src->reg; e.rm = dst; need_rex_for(&e, src); need_rex_for(&e, dst); encode(&e); return;
  }
  if (src->kind == O_MEM && dst->kind == O_REG) {
    if (src->gotpcrel) { if (size != 8) asm_error("GOTPCREL load into a non-64-bit register"); e.op[0] = 0x8D; e.oplen = 1; e.w = true; e.reg = dst->reg; e.rm = src; encode(&e); return; }
    set_size(&e, size, 0x8A, 0x8B); e.reg = dst->reg; e.rm = src; need_rex_for(&e, dst); encode(&e); return;
  }
  asm_error("unsupported mov operands");
}

// movs*/movz* family: mnemonic gives sign and source width; destination width from register
static void movx(char *mn, Op *src, Op *dst) {
  bool sign = mn[3] == 's';
  int srcsize;
  char *rest = mn + 4;   // after "movs"/"movz"
  if (!strcmp(mn, "movsxd") || !strcmp(mn, "movslq")) srcsize = 4;
  else if (*rest == 'b') srcsize = 1;
  else if (*rest == 'w') srcsize = 2;
  else if (*rest == 'x' || *rest == 0) { // movzx / movzb without suffix: source size from register
    if (src->kind != O_REG) asm_error("%s needs a register source", mn); srcsize = src->size;
  } else asm_error("unknown extension %s", mn);
  if (dst->kind != O_REG) asm_error("%s needs a register destination", mn);
  Enc e = {0};
  if (srcsize == 4) { e.w = true; e.op[0] = 0x63; e.oplen = 1; }
  else { e.op[0] = 0x0F; e.op[1] = (sign ? 0xBE : 0xB6) + (srcsize == 2 ? 1 : 0); e.oplen = 2; e.w = dst->size == 8; e.opsize16 = dst->size == 2; }
  e.reg = dst->reg; e.rm = src; need_rex_for(&e, src);
  encode(&e);
}

static int cc_code(char *s) {
  static struct { char *n; int c; } t[] = {
    {"o",0},{"no",1},{"b",2},{"c",2},{"nae",2},{"ae",3},{"nb",3},{"e",4},{"z",4},{"ne",5},{"nz",5},{"be",6},{"na",6},
    {"a",7},{"nbe",7},{"s",8},{"ns",9},{"p",10},{"np",11},{"l",12},{"ge",13},{"le",14},{"g",15},
  };
  for (int i = 0; i < sizeof(t) / sizeof(*t); i++) if (!strcmp(t[i].n, s)) return t[i].c;
  return -1;
}

static void sse(int prefix, uint8_t op, Op *src, Op *dst, bool w, bool xmm_in_reg_field_is_dst) {
  // generic "op xmm/m, xmm" form: reg field = dst xmm, rm = src
  Enc e = {0};
  e.prefix = prefix; e.op[0] = 0x0F; e.op[1] = op; e.oplen = 2; e.w = w;
  if (xmm_in_reg_field_is_dst) { e.reg = dst->reg; e.rm = src; }
  else { e.reg = src->reg; e.rm = dst; }
  encode(&e);
}

static void instruction(char *mn, char *rest) {
  Op ops[3]; int n = 0;
  if (rest && *rest) {
    char *p = rest;
    while (n < 3) {
      char *comma = strchr(p, ',');
      if (comma) *comma = 0;
      ops[n++] = parse_op(p);
      if (!comma) break;
      p = comma + 1;
    }
  }
  Op *a = n > 0 ? &ops[0] : NULL, *b = n > 1 ? &ops[1] : NULL;
  size_t L = strlen(mn);
  char suffix = 0;

  // prefixes
  if (!strcmp(mn, "rep")) {
    if (!strcmp(trim(rest), "stosb")) { emit8(0xF3); emit8(0xAA); return; }
    asm_error("unsupported rep instruction");
  }
  if (!strcmp(mn, "lock") || !strcmp(mn, "data16") || !strcmp(mn, "rex64")) asm_error("atomics/TLS are not supported");

  // no-operand instructions
  if (n == 0) {
    if (!strcmp(mn, "ret")) { emit8(0xC3); return; }
    if (!strcmp(mn, "cqo") || !strcmp(mn, "cqto")) { emit8(0x48); emit8(0x99); return; }
    if (!strcmp(mn, "cdq") || !strcmp(mn, "cltd")) { emit8(0x99); return; }
    if (!strcmp(mn, "syscall")) { emit8(0x0F); emit8(0x05); return; }
    if (!strcmp(mn, "hlt")) { emit8(0xF4); return; }
    if (!strcmp(mn, "nop")) { emit8(0x90); return; }
    if (mn[0] == 'f') asm_error("x87 (long double) is not supported");
    asm_error("unknown instruction %s", mn);
  }

  // control flow
  if (!strcmp(mn, "jmp") || !strcmp(mn, "call")) {
    if (a->kind == O_LABEL) { emit8(!strcmp(mn, "jmp") ? 0xE9 : 0xE8); add_fixup(a->sym, -4, false); emit32(0); return; }
    if (a->kind == O_REG && a->size == 8) { Enc e = {0}; e.op[0] = 0xFF; e.oplen = 1; e.reg = !strcmp(mn, "jmp") ? 4 : 2; e.rm = a; encode(&e); return; }
    asm_error("bad %s operand", mn);
  }
  if (mn[0] == 'j' && a->kind == O_LABEL) {
    int cc = cc_code(mn + 1);
    if (cc < 0) asm_error("unknown jump %s", mn);
    emit8(0x0F); emit8(0x80 + cc); add_fixup(a->sym, -4, false); emit32(0); return;
  }
  if (!strncmp(mn, "set", 3) && n == 1) {
    int cc = cc_code(mn + 3);
    if (cc < 0 || a->kind != O_REG || a->size != 1) asm_error("bad setcc");
    Enc e = {0}; e.op[0] = 0x0F; e.op[1] = 0x90 + cc; e.oplen = 2; e.reg = 0; e.rm = a; need_rex_for(&e, a); encode(&e); return;
  }

  // SSE
  if (!strcmp(mn, "movss") || !strcmp(mn, "movsd")) {
    int pfx = mn[4] == 's' ? 0xF3 : 0xF2;
    if (b->kind == O_REG && b->xmm) { sse(pfx, 0x10, a, b, false, true); return; }       // load / reg-reg
    if (a->kind == O_REG && a->xmm && b->kind == O_MEM) { sse(pfx, 0x11, a, b, false, false); return; } // store
    asm_error("bad %s operands", mn);
  }
  if (!strcmp(mn, "movq") && ((a->kind == O_REG && a->xmm) || (b->kind == O_REG && b->xmm))) {
    if (b->xmm) { Enc e = {0}; e.prefix = 0x66; e.w = true; e.op[0] = 0x0F; e.op[1] = 0x6E; e.oplen = 2; e.reg = b->reg; e.rm = a; encode(&e); return; }
    Enc e = {0}; e.prefix = 0x66; e.w = true; e.op[0] = 0x0F; e.op[1] = 0x7E; e.oplen = 2; e.reg = a->reg; e.rm = b; encode(&e); return;
  }
  if (!strncmp(mn, "cvtsi2s", 7)) {  // cvtsi2ss[lq] / cvtsi2sd[lq]: int -> float
    int pfx = mn[7] == 's' ? 0xF3 : 0xF2;
    bool w = mn[8] == 'q' || (mn[8] == 0 && a->kind == O_REG && a->size == 8);
    sse(pfx, 0x2A, a, b, w, true); return;
  }
  if (!strncmp(mn, "cvtts", 5)) {    // cvttss2si[lq] / cvttsd2si[lq]: float -> int
    int pfx = mn[5] == 's' ? 0xF3 : 0xF2;
    bool w = b->size == 8;
    Enc e = {0}; e.prefix = pfx; e.op[0] = 0x0F; e.op[1] = 0x2C; e.oplen = 2; e.w = w; e.reg = b->reg; e.rm = a; encode(&e); return;
  }
  if (!strcmp(mn, "cvtss2sd")) { sse(0xF3, 0x5A, a, b, false, true); return; }
  if (!strcmp(mn, "cvtsd2ss")) { sse(0xF2, 0x5A, a, b, false, true); return; }
  if (!strcmp(mn, "ucomiss")) { sse(0, 0x2E, a, b, false, true); return; }
  if (!strcmp(mn, "ucomisd")) { sse(0x66, 0x2E, a, b, false, true); return; }
  if (!strcmp(mn, "xorps")) { sse(0, 0x57, a, b, false, true); return; }
  if (!strcmp(mn, "xorpd")) { sse(0x66, 0x57, a, b, false, true); return; }
  if (!strcmp(mn, "pxor")) { sse(0x66, 0xEF, a, b, false, true); return; }
  {
    static struct { char *n; int op; } arith[] = {{"add",0x58},{"mul",0x59},{"sub",0x5C},{"div",0x5E}};
    for (int i = 0; i < 4; i++)
      if (!strncmp(mn, arith[i].n, 3) && L == 5 && mn[3] == 's' && (mn[4] == 's' || mn[4] == 'd')) {
        sse(mn[4] == 's' ? 0xF3 : 0xF2, arith[i].op, a, b, false, true); return;
      }
  }
  if (mn[0] == 'f') asm_error("x87 (long double) is not supported");

  // movs*/movz*
  if (!strncmp(mn, "movs", 4) || !strncmp(mn, "movz", 4)) {
    if (n != 2) asm_error("bad %s", mn);
    if (!strcmp(mn, "movsd") || !strcmp(mn, "movss")) asm_error("unreachable");
    movx(mn, a, b); return;
  }

  // integer mov / alu with optional size suffix
  char stem[16];
  strcpy(stem, mn);
  if (L > 3 && strchr("bwlq", mn[L - 1]) && (!strncmp(mn, "mov", 3) || !strncmp(mn, "add", 3) || !strncmp(mn, "sub", 3) ||
      !strncmp(mn, "and", 3) || !strncmp(mn, "cmp", 3) || !strncmp(mn, "xor", 3) || !strncmp(mn, "or", 2) ||
      !strncmp(mn, "test", 4) || !strncmp(mn, "imul", 4) || !strncmp(mn, "idiv", 4) || !strncmp(mn, "div", 3) ||
      !strncmp(mn, "neg", 3) || !strncmp(mn, "not", 3) || !strncmp(mn, "shl", 3) || !strncmp(mn, "shr", 3) || !strncmp(mn, "sar", 3) ||
      !strncmp(mn, "push", 4) || !strncmp(mn, "pop", 3) || !strncmp(mn, "lea", 3) || !strncmp(mn, "inc", 3) || !strncmp(mn, "dec", 3))) {
    if (strcmp(mn, "mul") && strcmp(mn, "shl") && strcmp(mn, "shr") && strcmp(mn, "sarl") ) {
      // only strip when the stem is a known mnemonic (avoid "mul"->"mu")
      static char *stems[] = {"mov","add","sub","and","cmp","xor","or","test","imul","idiv","div","neg","not","shl","shr","sar","push","pop","lea","inc","dec"};
      for (int i = 0; i < sizeof(stems) / sizeof(*stems); i++)
        if (strlen(stems[i]) == L - 1 && !strncmp(mn, stems[i], L - 1)) { suffix = mn[L - 1]; stem[L - 1] = 0; break; }
    }
  }
  mn = stem;

  if (!strcmp(mn, "mov")) { if (n != 2) asm_error("bad mov"); mov(a, b, suffix); return; }
  if (!strcmp(mn, "lea")) {
    if (n != 2 || a->kind != O_MEM || b->kind != O_REG) asm_error("bad lea");
    Enc e = {0}; e.op[0] = 0x8D; e.oplen = 1; e.w = b->size == 8; e.opsize16 = b->size == 2; e.reg = b->reg; e.rm = a; encode(&e); return;
  }
  {
    static struct { char *n; int g; } g1[] = {{"add",0},{"or",1},{"and",4},{"sub",5},{"xor",6},{"cmp",7}};
    for (int i = 0; i < 6; i++) if (!strcmp(mn, g1[i].n)) { if (n != 2) asm_error("bad %s", mn); alu(g1[i].g, a, b, suffix); return; }
  }
  if (!strcmp(mn, "test")) {
    if (n != 2 || a->kind != O_REG) asm_error("bad test");
    Enc e = {0}; int size = op_size(a, b, suffix); set_size(&e, size, 0x84, 0x85); e.reg = a->reg; e.rm = b; need_rex_for(&e, a); need_rex_for(&e, b); encode(&e); return;
  }
  if (!strcmp(mn, "push") || !strcmp(mn, "pop")) {
    if (n != 1 || a->kind != O_REG || a->size != 8) asm_error("bad %s", mn);
    if (a->reg >= 8) emit8(0x41);
    emit8((!strcmp(mn, "push") ? 0x50 : 0x58) + (a->reg & 7)); return;
  }
  if (!strcmp(mn, "imul")) {
    if (n != 2 || b->kind != O_REG) asm_error("bad imul");
    Enc e = {0}; e.op[0] = 0x0F; e.op[1] = 0xAF; e.oplen = 2; e.w = b->size == 8; e.opsize16 = b->size == 2; e.reg = b->reg; e.rm = a; encode(&e); return;
  }
  {
    static struct { char *n; int g; } g3[] = {{"not",2},{"neg",3},{"mul",4},{"imul",5},{"div",6},{"idiv",7}};
    for (int i = 0; i < 6; i++) if (!strcmp(mn, g3[i].n)) {
      if (n != 1) asm_error("bad %s", mn);
      Enc e = {0}; int size = op_size(a, NULL, suffix); set_size(&e, size, 0xF6, 0xF7); e.reg = g3[i].g; e.rm = a; need_rex_for(&e, a); encode(&e); return;
    }
  }
  if (!strcmp(mn, "inc") || !strcmp(mn, "dec")) {
    if (n != 1) asm_error("bad %s", mn);
    Enc e = {0}; int size = op_size(a, NULL, suffix); set_size(&e, size, 0xFE, 0xFF); e.reg = !strcmp(mn, "inc") ? 0 : 1; e.rm = a; need_rex_for(&e, a); encode(&e); return;
  }
  {
    static struct { char *n; int g; } sh[] = {{"shl",4},{"shr",5},{"sar",7},{"sal",4}};
    for (int i = 0; i < 4; i++) if (!strcmp(mn, sh[i].n)) {
      Enc e = {0};
      Op *dst = n == 2 ? b : a;
      int size = op_size(dst, NULL, suffix);
      if (n == 2 && a->kind == O_REG && a->reg == 1 && a->size == 1) { set_size(&e, size, 0xD2, 0xD3); e.reg = sh[i].g; e.rm = dst; need_rex_for(&e, dst); encode(&e); return; }
      if (n == 2 && a->kind == O_IMM) { set_size(&e, size, 0xC0, 0xC1); e.reg = sh[i].g; e.rm = dst; e.imm_size = 1; e.imm = a->imm; need_rex_for(&e, dst); encode(&e); return; }
      if (n == 1) { set_size(&e, size, 0xD0, 0xD1); e.reg = sh[i].g; e.rm = dst; need_rex_for(&e, dst); encode(&e); return; }
      asm_error("bad shift");
    }
  }
  if (!strcmp(mn, "xchg") || !strcmp(mn, "cmpxchg")) asm_error("atomics are not supported");
  asm_error("unknown instruction %s", mn);
}

static void directive(char *d, char *rest) {
  if (!strcmp(d, ".text")) { cur = &text; cur_bss = false; return; }
  if (!strcmp(d, ".data")) { cur = &data; cur_bss = false; return; }
  if (!strcmp(d, ".bss")) { cur = &data; cur_bss = true; return; }
  if (!strcmp(d, ".section")) {
    rest = trim(rest);
    if (!strncmp(rest, ".text", 5)) { cur = &text; cur_bss = false; return; }
    if (!strncmp(rest, ".data", 5)) { cur = &data; cur_bss = false; return; }
    if (!strncmp(rest, ".bss", 4)) { cur = &data; cur_bss = true; return; }
    asm_error("unsupported section %s (TLS?)", rest);
  }
  if (!strcmp(d, ".globl") || !strcmp(d, ".local") || !strcmp(d, ".type") || !strcmp(d, ".size") ||
      !strcmp(d, ".file") || !strcmp(d, ".loc")) return;
  if (!strcmp(d, ".align")) {
    long a = strtol(rest, NULL, 10);
    if (a <= 0 || (a & (a - 1))) asm_error("bad alignment");
    if (cur_bss) { bss_len = align_to(bss_len, a); return; }
    while (cur->len % a) emit8(cur == &text ? 0x90 : 0);
    return;
  }
  if (cur_bss && (!strcmp(d, ".byte") || !strcmp(d, ".quad"))) asm_error("data in .bss");
  if (!strcmp(d, ".byte")) { long v = strtol(rest, NULL, 10); emit8((int)v & 0xff); return; }
  if (!strcmp(d, ".zero")) { long v = strtol(rest, NULL, 10); if (cur_bss) bss_len += v; else for (long i = 0; i < v; i++) emit8(0); return; }
  if (!strcmp(d, ".quad")) {
    rest = trim(rest);
    if (isdigit(*rest) || *rest == '-') { emit64((uint64_t)strtoll(rest, NULL, 10)); return; }
    char *plus = strpbrk(rest, "+-");
    long addend = 0;
    if (plus) { addend = strtol(plus, NULL, 10); *plus = 0; }
    add_fixup(strdup(trim(rest)), addend, true);
    emit64(0);
    return;
  }
  if (!strcmp(d, ".comm")) {  // .comm name, size, align
    char *name = strtok(rest, ","); char *sz = strtok(NULL, ","); char *al = strtok(NULL, ",");
    if (!name || !sz || !al) asm_error("bad .comm");
    long size = strtol(sz, NULL, 10), a = strtol(al, NULL, 10);
    bss_len = align_to(bss_len, a);
    Sym *s = calloc(1, sizeof(Sym)); s->sec = 2; s->off = bss_len;
    hashmap_put(&syms, strdup(trim(name)), s);
    bss_len += size;
    return;
  }
  asm_error("unknown directive %s", d);
}

static void assemble_line(char *line) {
  cur_line = line;
  char *s = trim(line);
  if (!*s) return;
  // label?
  size_t L = strlen(s);
  if (s[L - 1] == ':') {
    s[L - 1] = 0;
    if (isdigit(*s) && !s[1]) { int k = ++numeric_count[*s - '0']; define_sym(numeric_name(*s - '0', k)); return; }
    define_sym(strdup(s));
    return;
  }
  char *sp = s;
  while (*sp && !isspace(*sp)) sp++;
  char *rest = *sp ? sp + 1 : sp;
  *sp = 0;
  if (*s == '.') directive(s, rest);
  else if (cur_bss) asm_error("instruction in .bss");
  else instruction(s, rest);
}

// ---- ELF ----
#define BASE 0x400000UL
#define PAGE 0x1000UL

static void put16(uint8_t *p, uint64_t v) { for (int i = 0; i < 2; i++) p[i] = (v >> (8 * i)) & 0xff; }
static void put32(uint8_t *p, uint64_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xff; }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (v >> (8 * i)) & 0xff; }

void assemble_elf(char *asm_text, FILE *out) {
  cur = &text; cur_bss = false;
  char *p = asm_text;
  line_no = 0;
  while (*p) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = 0;
    line_no++;
    // chibicc's cast strings pack several instructions on one line, separated by ';'
    char *seg = p;
    while (seg) {
      char *semi = strchr(seg, ';');
      if (semi) *semi = 0;
      assemble_line(seg);
      seg = semi ? semi + 1 : NULL;
    }
    if (!nl) break;
    p = nl + 1;
  }

  // layout
  uint64_t text_off = PAGE, text_addr = BASE + text_off;
  uint64_t data_off = align_to(text_off + text.len, PAGE), data_addr = BASE + data_off;
  uint64_t bss_addr = align_to(data_addr + data.len, 16);
  uint64_t sec_addr[3] = { text_addr, data_addr, bss_addr };

  // resolve fixups
  for (int i = 0; i < nfixups; i++) {
    Fixup *f = &fixups[i];
    Sym *s = hashmap_get(&syms, f->name);
    if (!s) error("asm: undefined symbol %s", f->name);
    uint64_t target = sec_addr[s->sec] + s->off + f->addend;
    Sec *sec = f->sec == 0 ? &text : &data;
    if (f->abs64) { put64(sec->data + f->off, target); continue; }
    int64_t rel = (int64_t)target - (int64_t)(sec_addr[f->sec] + f->off);   // addend already includes -4 (and -imm)
    if (rel < INT32_MIN || rel > INT32_MAX) error("asm: relocation out of range for %s", f->name);
    put32(sec->data + f->off, (uint32_t)(int32_t)rel);
  }

  Sym *start = hashmap_get(&syms, "_start");
  if (!start || start->sec != 0) error("asm: no _start in .text");
  uint64_t entry = text_addr + start->off;

  uint8_t hdr[64 + 2 * 56] = {0};
  memcpy(hdr, "\177ELF\2\1\1", 7);
  put16(hdr + 16, 2); put16(hdr + 18, 62); put32(hdr + 20, 1);
  put64(hdr + 24, entry); put64(hdr + 32, 64); put64(hdr + 40, 0);
  put32(hdr + 48, 0); put16(hdr + 52, 64); put16(hdr + 54, 56); put16(hdr + 56, 2);
  put16(hdr + 58, 64); put16(hdr + 60, 0); put16(hdr + 62, 0);
  uint8_t *ph = hdr + 64;
  put32(ph, 1); put32(ph + 4, 5); put64(ph + 8, 0); put64(ph + 16, BASE); put64(ph + 24, BASE);
  put64(ph + 32, text_off + text.len); put64(ph + 40, text_off + text.len); put64(ph + 48, PAGE);
  ph += 56;
  put32(ph, 1); put32(ph + 4, 6); put64(ph + 8, data_off); put64(ph + 16, data_addr); put64(ph + 24, data_addr);
  put64(ph + 32, data.len); put64(ph + 40, (bss_addr - data_addr) + bss_len); put64(ph + 48, PAGE);

  fwrite(hdr, 1, sizeof hdr, out);
  for (uint64_t i = sizeof hdr; i < text_off; i++) fputc(0, out);
  fwrite(text.data, 1, text.len, out);
  for (uint64_t i = text_off + text.len; i < data_off; i++) fputc(0, out);
  fwrite(data.data, 1, data.len, out);
}
