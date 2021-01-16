//
// compiler.h - Header file for BP compiler.
//
#ifndef COMPILER__H
#define COMPILER__H

#include "files.h"
#include "types.h"

__attribute__((nonnull))
pat_t *new_pat(file_t *f, const char *start, enum pattype_e type);
__attribute__((nonnull(1,2)))
pat_t *bp_simplepattern(file_t *f, const char *str);
__attribute__((nonnull(1,2)))
pat_t *bp_stringpattern(file_t *f, const char *str);
__attribute__((nonnull(1,2)))
pat_t *bp_replacement(file_t *f, pat_t *replacepat, const char *replacement);
__attribute__((nonnull))
pat_t *bp_pattern(file_t *f, const char *str);
__attribute__((nonnull))
def_t *bp_definition(file_t *f, const char *str);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
