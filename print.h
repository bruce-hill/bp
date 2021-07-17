//
// Header file for print.c (printing/visualizing matches)
//
#ifndef PRINT__H
#define PRINT__H

#include <stdbool.h>

#include "types.h"

typedef struct {
    file_t *file;
    const char *pos;
    int context_lines;
    bool needs_line_number:1;
    bool use_color:1;
    bool print_line_numbers:1;
} printer_t;

__attribute__((nonnull(1,2)))
void print_match(FILE *out, printer_t *pr, match_t *m);
__attribute__((nonnull))
int print_errors(printer_t *pr, match_t *m);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
