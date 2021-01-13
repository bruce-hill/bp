//
// Header file for printing.c (printing/visualizing matches)
//
#ifndef PRINTING__H
#define PRINTING__H

#include "types.h"

typedef enum {
    PRINT_COLOR = 1<<0,
    PRINT_LINE_NUMBERS = 1<<1,
} print_options_t;

__attribute__((nonnull))
void visualize_match(match_t *m);
__attribute__((nonnull))
void print_match(FILE *out, file_t *f, match_t *m, print_options_t options);
__attribute__((nonnull))
int print_errors(file_t *f, match_t *m, print_options_t options);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
