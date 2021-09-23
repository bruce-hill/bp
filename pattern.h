//
// pattern.h - Header file for BP pattern compilation.
//
#ifndef PATTERN__H
#define PATTERN__H

#include "files.h"
#include "types.h"

typedef struct {
    bool success;
    union {
        pat_t *pat;
        struct {
            const char *start, *end, *msg;
        } error;
    } value;
} maybe_pat_t;

__attribute__((nonnull, returns_nonnull))
pat_t *bp_backref(file_t *f, match_t *m);
__attribute__((nonnull))
maybe_pat_t bp_stringpattern(file_t *f, const char *str);
__attribute__((nonnull(1,2)))
maybe_pat_t bp_replacement(file_t *f, pat_t *replacepat, const char *replacement);
__attribute__((nonnull(1)))
pat_t *chain_together(file_t *f, pat_t *first, pat_t *second);
__attribute__((nonnull(1)))
pat_t *either_pat(file_t *f, pat_t *first, pat_t *second);
__attribute__((nonnull))
maybe_pat_t bp_pattern(file_t *f, const char *str);


#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
