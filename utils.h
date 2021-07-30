//
// utils.h - Some utility and printing functions.
//
#ifndef UTILS__H
#define UTILS__H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "match.h"

#define S1(x) #x
#define S2(x) S1(x)
#define __LOCATION__ __FILE__ ":" S2(__LINE__)

#define streq(a, b) (strcmp(a, b) == 0)
#define new(t) check_nonnull(calloc(1, sizeof(t)), __LOCATION__ ": `new(" #t ")` allocation failure")
#define checked_strdup(s) check_nonnull(strdup(s), __LOCATION__ ": `checked_strdup(" #s ")` allocation failure")
#define grow(arr,n) check_nonnull(realloc(arr,sizeof(arr[0])*(n)), __LOCATION__ ": `groaw(" #arr ", " #n ")` allocation failure")

__attribute__((nonnull(1)))
char unescapechar(const char *escaped, const char **end);
__attribute__((pure, nonnull))
const char *after_name(const char *str);
__attribute__((pure, nonnull, returns_nonnull))
const char *after_spaces(const char *str);
__attribute__((nonnull))
bool matchchar(const char **str, char c);
__attribute__((nonnull))
bool matchstr(const char **str, const char *target);
__attribute__((returns_nonnull))
void *check_nonnull(void *p, const char *err_msg, ...);
int check_nonnegative(int i, const char *err_msg, ...);
__attribute__((nonnull))
int memicmp(const void *s1, const void *s2, size_t n);
__attribute__((nonnull))
void delete(void *p);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
