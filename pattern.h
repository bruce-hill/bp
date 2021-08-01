//
// pattern.h - Header file for BP pattern compilation.
//
#ifndef PATTERN__H
#define PATTERN__H

#include "files.h"
#include "types.h"

__attribute__((returns_nonnull, nonnull(1,2)))
pat_t *new_pat(file_t *f, const char *start, const char *end, size_t minlen, ssize_t maxlen, enum pattype_e type);
__attribute__((nonnull(1,2)))
pat_t *bp_stringpattern(file_t *f, const char *str);
__attribute__((nonnull(1,2)))
pat_t *bp_replacement(file_t *f, pat_t *replacepat, const char *replacement);
__attribute__((nonnull(1)))
pat_t *chain_together(file_t *f, pat_t *first, pat_t *second);
__attribute__((nonnull(1)))
pat_t *either_pat(file_t *f, pat_t *first, pat_t *second);
__attribute__((nonnull))
pat_t *bp_pattern(file_t *f, const char *str);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
