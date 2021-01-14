//
// vm.h - Header file for BP virtual machine.
//
#ifndef VM__H
#define VM__H

#include <stdio.h>

#include "types.h"

__attribute__((nonnull(2,3,4)))
match_t *match(def_t *defs, file_t *f, const char *str, vm_op_t *op, unsigned int flags);
__attribute__((nonnull))
void destroy_match(match_t **m);
__attribute__((nonnull))
match_t *get_capture(match_t *m, const char **id);
__attribute__((nonnull))
void destroy_op(vm_op_t *op);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
