#ifndef _STDARG_H
#define _STDARG_H
#if defined(__chibicc__) && defined(__x86_64__)
/* chibicc x86-64 target: upstream chibicc's va_list over the register save area */
typedef struct {
  unsigned int gp_offset;
  unsigned int fp_offset;
  void *overflow_arg_area;
  void *reg_save_area;
} __va_elem;

typedef __va_elem va_list[1];

#define va_start(ap, last) \
  do { *(ap) = *(__va_elem *)__va_area__; } while (0)

#define va_end(ap)

static void *__va_arg_mem(__va_elem *ap, int sz, int align) {
  void *p = ap->overflow_arg_area;
  if (align > 8)
    p = (p + 15) / 16 * 16;
  ap->overflow_arg_area = ((unsigned long)p + sz + 7) / 8 * 8;
  return p;
}

static void *__va_arg_gp(__va_elem *ap, int sz, int align) {
  if (ap->gp_offset >= 48)
    return __va_arg_mem(ap, sz, align);

  void *r = ap->reg_save_area + ap->gp_offset;
  ap->gp_offset += 8;
  return r;
}

static void *__va_arg_fp(__va_elem *ap, int sz, int align) {
  if (ap->fp_offset >= 112)
    return __va_arg_mem(ap, sz, align);

  void *r = ap->reg_save_area + ap->fp_offset;
  ap->fp_offset += 8;
  return r;
}

#define va_arg(ap, ty)                                                  \
  ({                                                                    \
    int klass = __builtin_reg_class(ty);                                \
    *(ty *)(klass == 0 ? __va_arg_gp(ap, sizeof(ty), _Alignof(ty)) :    \
            klass == 1 ? __va_arg_fp(ap, sizeof(ty), _Alignof(ty)) :    \
            __va_arg_mem(ap, sizeof(ty), _Alignof(ty)));                \
  })

#define va_copy(dest, src) ((dest)[0] = (src)[0])

#define __GNUC_VA_LIST 1
typedef va_list __gnuc_va_list;

#elif defined(__chibicc__)
/* chibicc-wasm ABI: variadic arguments live in 8-byte slots behind a pointer */
typedef char* va_list;
#define va_start(ap, last) ((ap) = __va_area__)
#define va_end(ap) ((void)0)
#define va_arg(ap, t) (*(t*)(((ap) += 8) - 8))
#else
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap) __builtin_va_end(ap)
#define va_arg(ap, t) __builtin_va_arg(ap, t)
#endif
#endif
