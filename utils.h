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

#define streq(a, b) (strcmp(a, b) == 0)
#define check(cond, ...) do { if (!(cond)) { (void)fprintf(stderr, __VA_ARGS__); (void)fwrite("\n", 1, 1, stderr); exit(EXIT_FAILURE); } } while(false)
#define new(t) memcheck(calloc(1, sizeof(t)))
#define xcalloc(a,b) memcheck(calloc(a,b))
#define xrealloc(a,b) memcheck(realloc(a,b))

__attribute__((nonnull))
char unescapechar(const char *escaped, const char **end);
__attribute__((pure, nonnull))
const char *after_name(const char *str);
__attribute__((pure, nonnull, returns_nonnull))
const char *after_spaces(const char *str);
__attribute__((nonnull))
bool matchchar(const char **str, char c);
__attribute__((nonnull))
bool matchstr(const char **str, const char *target);
__attribute__((nonnull))
size_t unescape_string(char *dest, const char *src, size_t bufsize);
__attribute__((returns_nonnull))
void *memcheck(/*@null@*/ /*@out@*/ void *p);
__attribute__((nonnull))
int memicmp(const void *s1, const void *s2, size_t n);
__attribute__((nonnull))
void xfree(void *p);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
