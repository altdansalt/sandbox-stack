#ifndef _STDDEF_H
#define _STDDEF_H
typedef unsigned long size_t;
typedef long ptrdiff_t;
#define NULL ((void*)0)
#ifdef __chibicc__
#define offsetof(t, m) ((size_t)&(((t*)0)->m))
#else
#define offsetof(t, m) __builtin_offsetof(t, m)
#endif
#endif
