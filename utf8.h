//
// utf8.h - UTF8 helper functions
//
#include "files.h"

#ifndef UTF8__H
#define UTF8__H

#define UTF8_MAXCHARLEN 4

__attribute__((nonnull, pure))
const char *next_char(file_t *f, const char *str);
__attribute__((nonnull, pure))
const char *prev_char(file_t *f, const char *str);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
