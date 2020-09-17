/*
 * vm.h - Header file for BPEG virtual machine.
 */
#ifndef VM__H
#define VM__H

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

#include "types.h"

const char *opcode_name(enum VMOpcode o);
match_t *match(grammar_t *g, file_t *f, const char *str, vm_op_t *op, unsigned int flags);
void destroy_match(match_t **m);
void print_pattern(vm_op_t *op);
void print_match(file_t *f, match_t *m, const char *color, int verbose);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
