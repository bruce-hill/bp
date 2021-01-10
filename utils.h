/*
 * utils.h - Some utility and printing functions.
 */
#ifndef UTILS__H
#define UTILS__H

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vm.h"

#define streq(a, b) (strcmp(a, b) == 0)
// TODO: better error reporting
#define check(cond, ...) do { if (!(cond)) { fprintf(stderr, __VA_ARGS__); fwrite("\n", 1, 1, stderr); _exit(1); } } while(0)
#define debug(...) do { if (verbose) fprintf(stderr, __VA_ARGS__); } while(0)
#define new(t) memcheck(calloc(sizeof(t), 1))
#define xcalloc(a,b) memcheck(calloc(a,b))
#define xrealloc(a,b) memcheck(realloc(a,b))

__attribute__((nonnull))
unsigned char unescapechar(const char *escaped, const char **end);
__attribute__((pure, nonnull))
const char *after_name(const char *str);
__attribute__((pure, nonnull, returns_nonnull))
const char *after_spaces(const char *str);
__attribute__((nonnull))
int matchchar(const char **str, char c);
__attribute__((nonnull))
int matchstr(const char **str, const char *target);
__attribute__((nonnull))
size_t unescape_string(char *dest, const char *src, size_t bufsize);
void *memcheck(void *p);
__attribute__((nonnull))
int memicmp(const void *s1, const void *s2, size_t n);
__attribute__((nonnull))
void xfree(void **p);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
