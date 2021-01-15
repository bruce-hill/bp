//
// vm.h - Header file for BP virtual machine.
//
#ifndef VM__H
#define VM__H

#include <stdio.h>

#include "types.h"

__attribute__((nonnull(2,4)))
match_t *next_match(def_t *defs, file_t *f, match_t *prev, vm_op_t *op, unsigned int flags);
__attribute__((hot, nonnull(2,3,4)))
match_t *match(def_t *defs, file_t *f, const char *str, vm_op_t *op, unsigned int flags);
__attribute__((nonnull))
match_t *get_capture(match_t *m, const char **id);
__attribute__((nonnull))
void destroy_op(vm_op_t *op);
match_t *new_match(void);
__attribute__((nonnull))
void recycle_if_unused(match_t **at_m);
#ifdef DEBUG_HEAP
size_t free_all_matches(void);
size_t recycle_all_matches(void);
#endif

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
