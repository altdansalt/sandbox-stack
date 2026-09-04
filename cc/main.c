// Driver for the wasm backend: one translation unit in, one .wasm out.
#include "chibicc.h"

StringArray include_paths;
bool opt_fpic;
bool opt_fcommon = false;
bool opt_x86;
char *base_file;

static bool opt_E;
static bool opt_asm;   // input is x86-64 assembly text (assembler tests)
bool opt_S;   // x86: write assembly text instead of an ELF executable (debugging aid)
static char *opt_o;
static char *input_path;

bool file_exists(char *path) {
  struct stat st;
  return !stat(path, &st);
}

static void define_arg(char *str) {
  char *eq = strchr(str, '=');
  if (eq)
    define_macro(strndup(str, eq - str), eq + 1);
  else
    define_macro(str, "1");
}

static int parse_num(char *s, long lo, long hi, char *opt) {
  char *end;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (errno || *end || !*s || v < lo || v > hi)
    error("%s: expected a number in [%ld, %ld], got \"%s\"", opt, lo, hi, s);
  return (int)v;
}

static void usage(int status) {
  fprintf(stderr, "usage: cc [-E] [-mx86] [-I<dir>] [-D<macro>[=val]] [-mstack=<bytes>] [-mmaxpages=<n>] -o <out> <input.c>\n"
                  "  default: wasm32 module; -mx86: static x86-64 ELF executable (no libc)\n");
  exit(status);
}

static void parse_args(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-o")) { if (!argv[++i]) usage(1); opt_o = argv[i]; continue; }
    if (!strncmp(argv[i], "-o", 2)) { opt_o = argv[i] + 2; continue; }
    if (!strcmp(argv[i], "-E")) { opt_E = true; continue; }
    if (!strcmp(argv[i], "-I")) { if (!argv[++i]) usage(1); strarray_push(&include_paths, argv[i]); continue; }
    if (!strncmp(argv[i], "-I", 2)) { strarray_push(&include_paths, argv[i] + 2); continue; }
    if (!strcmp(argv[i], "-D")) { if (!argv[++i]) usage(1); define_arg(argv[i]); continue; }
    if (!strncmp(argv[i], "-D", 2)) { define_arg(argv[i] + 2); continue; }
    if (!strncmp(argv[i], "-mstack=", 8)) { opt_stack_size = parse_num(argv[i] + 8, 16, 1 << 30, "-mstack"); continue; }
    if (!strncmp(argv[i], "-mmaxpages=", 11)) { opt_max_pages = parse_num(argv[i] + 11, 1, 65536, "-mmaxpages"); continue; }
    if (!strcmp(argv[i], "-mx86")) { opt_x86 = true; continue; }
    if (!strcmp(argv[i], "-S")) { opt_S = true; continue; }
    if (!strcmp(argv[i], "-masm")) { opt_asm = opt_x86 = true; continue; }
    if (!strcmp(argv[i], "--help")) usage(0);
    if (argv[i][0] == '-' && argv[i][1]) error("unknown argument: %s", argv[i]);
    if (input_path) error("only one input file is supported");
    input_path = argv[i];
  }
  if (!input_path) error("no input file");
}

static void print_tokens(Token *tok) {
  int line = 1;
  for (; tok->kind != TK_EOF; tok = tok->next) {
    if (line > 1 && tok->at_bol) printf("\n");
    if (tok->has_space && !tok->at_bol) printf(" ");
    printf("%.*s", tok->len, tok->loc);
    line++;
  }
  printf("\n");
}

int main(int argc, char **argv) {
  parse_args(argc, argv);
  if (opt_x86) set_target_lp64();
  init_macros();
  base_file = input_path;

  if (opt_asm) {
    FILE *in = fopen(input_path, "rb");
    if (!in) error("%s: %s", input_path, strerror(errno));
    char *buf = NULL; size_t len = 0, cap = 0; int c;
    while ((c = fgetc(in)) != EOF) { if (len + 2 > cap) { cap = cap ? cap * 2 : 4096; buf = realloc(buf, cap); } buf[len++] = (char)c; }
    if (!buf) error("empty input");
    buf[len] = 0;
    FILE *out = opt_o ? fopen(opt_o, "wb") : stdout;
    if (!out) error("cannot open output file: %s: %s", opt_o, strerror(errno));
    assemble_elf(buf, out);
    fclose(out);
    if (opt_o) chmod(opt_o, 0755);
    return 0;
  }

  Token *tok = tokenize_file(input_path);
  if (!tok) error("%s: %s", input_path, strerror(errno));
  tok = preprocess(tok);

  if (opt_E) { print_tokens(tok); return 0; }

  Obj *prog = parse(tok);
  FILE *out = opt_o ? fopen(opt_o, "wb") : stdout;
  if (!out) error("cannot open output file: %s: %s", opt_o, strerror(errno));
  if (opt_x86) codegen_x86(prog, out);
  else codegen(prog, out);
  fclose(out);
  if (opt_x86 && !opt_S && opt_o) chmod(opt_o, 0755);
  return 0;
}
