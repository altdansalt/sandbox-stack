#ifndef _STDARG_H
#define _STDARG_H
#ifdef __chibicc__
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
