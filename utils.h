//
// utils.h - Some utility and printing functions.
//
#pragma once

#include <err.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef auto
#define auto __auto_type
#endif

#define S1(x) #x
#define S2(x) S1(x)

#define require(e, msg) ({\
    __typeof__(e) __expr = e; \
    if (_Generic(__expr, int: (ssize_t)__expr < 0, ssize_t: (ssize_t)__expr < 0, default: !__expr)) errx(1, __FILE__":"S2(__LINE__)": " msg); \
    __expr; \
})

#define When(x, _tag) ((x)->type == _tag ? &(x)->__tagged._tag : (errx(1, __FILE__ ":%d This was supposed to be a " # _tag "\n", __LINE__), &(x)->__tagged._tag))

#ifndef public
#define public __attribute__ ((visibility ("default")))
#endif

#define new(t) require(calloc(1, sizeof(t)), "`new(" #t ")` allocation failure")
#define checked_strdup(s) require(strdup(s), "`checked_strdup(" #s ")` allocation failure")
#define grow(arr,n) require(realloc(arr,sizeof(arr[0])*(n)), "`grow(" #arr ", " #n ")` allocation failure")

#define streq(a, b) (strcmp(a, b) == 0)

__attribute__((nonnull(1)))
char unescapechar(const char *escaped, const char **after, const char *end);
__attribute__((pure, nonnull))
const char *after_name(const char *str, const char *end);
__attribute__((pure, nonnull, returns_nonnull))
const char *after_spaces(const char *str, bool skip_nl, const char *end);
__attribute__((nonnull))
bool matchchar(const char **str, char c, bool skip_nl, const char *end);
__attribute__((nonnull))
bool matchstr(const char **str, const char *target, bool skip_nl, const char *end);
__attribute__((nonnull))
void delete(void *p);

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
