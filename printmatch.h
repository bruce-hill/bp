//
// Debug visualization of matches
//
#ifndef EXPLAIN__H
#define EXPLAIN__H

#include "match.h"

typedef struct {
    const char *normal_color, *match_color, *replace_color;
    void (*fprint_between)(FILE *out, const char *start, const char *end, const char *normal_color);
    void (*on_nl)(FILE *out);
} print_options_t;
__attribute__((nonnull(1,2,3)))
//void fprint_match(FILE *out, const char *file_start, match_t *m, const char *colors[3]);
void fprint_match(FILE *out, const char *file_start, match_t *m, print_options_t *opts);

__attribute__((nonnull))
void explain_match(match_t *m);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
