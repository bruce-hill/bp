//
// utf8.h - UTF8 helper functions
//
#pragma once

#include <stdbool.h>

#define UTF8_MAXCHARLEN 4

__attribute__((nonnull, pure))
const char *next_char(const char *str, const char *end);
__attribute__((nonnull, pure))
const char *prev_char(const char *start, const char *str);
__attribute__((nonnull, pure))
bool isidstart(const char *str, const char *end);
__attribute__((nonnull, pure))
bool isidcontinue(const char *str, const char *end);

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
