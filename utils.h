//
// utils.h - Some utility and printing functions.
//
#ifndef UTILS__H
#define UTILS__H

#include <err.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "match.h"

#define S1(x) #x
#define S2(x) S1(x)
#define __LOCATION__ __FILE__ ":" S2(__LINE__)

#define DEFINE_CHECK_TYPE(t, name, var, expr) \
static inline t _check_##name(t var, const char *fmt, ...) { \
    if (!(expr)) {\
        va_list args;\
        va_start(args, fmt);\
        verrx(1, fmt, args);\
        va_end(args);\
    }\
    return var;\
}
DEFINE_CHECK_TYPE(void*, ptr, p, p);
DEFINE_CHECK_TYPE(int, int, i, i >= 0);
DEFINE_CHECK_TYPE(ssize_t, ssize_t, i, i >= 0);
DEFINE_CHECK_TYPE(char, char, c, c);
DEFINE_CHECK_TYPE(_Bool, bool, b, b);

#define PP_ARG_N(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,_17,_18,_19,_20,_21,_22,_23,_24,_25,_26,_27,_28,_29,_30,_31,N,...) N
#define PP_NARG(...) PP_ARG_N(__VA_ARGS__,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0)

#define _require_fmt(e, ...) _Generic((e), _Bool: _check_bool, int: _check_int, ssize_t: _check_ssize_t, char: _check_char, void*: _check_ptr, default: _check_ptr)((e), ""__VA_ARGS__)
#define require(e, ...) (PP_NARG(e,##__VA_ARGS__) > 1 ? _require_fmt((e), __LOCATION__": "__VA_ARGS__) : _require_fmt((e), __LOCATION__": `%s` failed", #e))
#define require_true(e, ...) (PP_NARG(e,##__VA_ARGS__) > 1 ? _require_fmt((_Bool)(e), __LOCATION__": "__VA_ARGS__) : _require_fmt((_Bool)(e), __LOCATION__": `%s` is not true", #e))

#define new(t) _check_ptr(calloc(1, sizeof(t)), "`new(" #t ")` allocation failure")
#define checked_strdup(s) _check_ptr(strdup(s), "`checked_strdup(" #s ")` allocation failure")
#define grow(arr,n) _check_ptr(realloc(arr,sizeof(arr[0])*(n)), "`grow(" #arr ", " #n ")` allocation failure")

#define streq(a, b) (strcmp(a, b) == 0)

__attribute__((nonnull(1)))
char unescapechar(const char *escaped, const char **end);
__attribute__((pure, nonnull))
const char *after_name(const char *str);
__attribute__((pure, nonnull, returns_nonnull))
const char *after_spaces(const char *str, bool skip_nl);
__attribute__((nonnull))
bool matchchar(const char **str, char c, bool skip_nl);
__attribute__((nonnull))
bool matchstr(const char **str, const char *target, bool skip_nl);
__attribute__((nonnull))
int memicmp(const void *s1, const void *s2, size_t n);
__attribute__((nonnull))
void delete(void *p);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
