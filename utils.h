/*
 * utils.h - Some utility and printing functions.
 */
#ifndef UTILS__H
#define UTILS__H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vm.h"

#define streq(a, b) (strcmp(a, b) == 0)
// TODO: better error reporting
#define check(cond, ...) do { if (!(cond)) { fprintf(stderr, __VA_ARGS__); fwrite("\n", 1, 1, stderr); _exit(1); } } while(0)
#define debug(...) do { if (verbose) fprintf(stderr, __VA_ARGS__); } while(0)

char *readfile(int fd);
void freefile(char *f);
char unescapechar(const char *escaped, const char **end);
const char *after_name(const char *str);
const char *after_spaces(const char *str);
int matchchar(const char **str, char c);
size_t unescape_string(char *dest, const char *src, size_t bufsize);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
