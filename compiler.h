//
// compiler.h - Header file for BP compiler.
//
#ifndef COMPILER__H
#define COMPILER__H

#include "file_loader.h"
#include "types.h"

__attribute__((nonnull))
vm_op_t *new_op(file_t *f, const char *start, enum VMOpcode type);
__attribute__((nonnull(1,2)))
vm_op_t *bp_simplepattern(file_t *f, const char *str);
__attribute__((nonnull(1,2)))
vm_op_t *bp_stringpattern(file_t *f, const char *str);
__attribute__((nonnull(1,2)))
vm_op_t *bp_replacement(file_t *f, vm_op_t *pat, const char *replacement);
__attribute__((nonnull))
vm_op_t *bp_pattern(file_t *f, const char *str);
__attribute__((nonnull))
def_t *bp_definition(file_t *f, const char *str);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
