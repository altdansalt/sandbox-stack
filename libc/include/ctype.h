#ifndef _CTYPE_H
#define _CTYPE_H
static __inline__ int isdigit(int c) { return c >= '0' && c <= '9'; }
static __inline__ int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static __inline__ int islower(int c) { return c >= 'a' && c <= 'z'; }
static __inline__ int isalpha(int c) { return isupper(c) || islower(c); }
static __inline__ int isalnum(int c) { return isalpha(c) || isdigit(c); }
static __inline__ int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static __inline__ int isprint(int c) { return c >= 32 && c < 127; }
static __inline__ int toupper(int c) { return islower(c) ? c - 32 : c; }
static __inline__ int tolower(int c) { return isupper(c) ? c + 32 : c; }
#endif
