/*
 * vm.h - Header file for BPEG virtual machine.
 */
#ifndef VM__H
#define VM__H

#include "types.h"

typedef enum {
    PRINT_COLOR = 1<<0,
    PRINT_LINE_NUMBERS = 1<<1,
} print_options_t;

const char *opcode_name(enum VMOpcode o);
__attribute__((hot, nonnull))
match_t *match(grammar_t *g, file_t *f, const char *str, vm_op_t *op, unsigned int flags);
__attribute__((nonnull))
void destroy_match(match_t **m);
__attribute__((nonnull))
void print_match(file_t *f, match_t *m, print_options_t options);
__attribute__((nonnull))
void json_match(FILE *f, const char *text, match_t *m, int verbose);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
