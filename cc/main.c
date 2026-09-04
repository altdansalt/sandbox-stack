// Driver for the wasm backend: one translation unit in, one .wasm out.
#include "chibicc.h"

StringArray include_paths;
bool opt_fpic;
bool opt_fcommon = true;
char *base_file;

static bool opt_E;
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

static void usage(int status) {
  fprintf(stderr, "usage: cc [-E] [-I<dir>] [-D<macro>[=val]] [-mstack=<bytes>] [-mmaxpages=<n>] -o <out.wasm> <input.c>\n");
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
    if (!strncmp(argv[i], "-mstack=", 8)) { opt_stack_size = atoi(argv[i] + 8); continue; }
    if (!strncmp(argv[i], "-mmaxpages=", 11)) { opt_max_pages = atoi(argv[i] + 11); continue; }
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
  init_macros();
  parse_args(argc, argv);
  base_file = input_path;

  Token *tok = tokenize_file(input_path);
  if (!tok) error("%s: %s", input_path, strerror(errno));
  tok = preprocess(tok);

  if (opt_E) { print_tokens(tok); return 0; }

  Obj *prog = parse(tok);
  FILE *out = opt_o ? fopen(opt_o, "wb") : stdout;
  if (!out) error("cannot open output file: %s: %s", opt_o, strerror(errno));
  codegen(prog, out);
  fclose(out);
  return 0;
}
