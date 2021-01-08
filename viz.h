/*
 * Header file for viz.c (visualizing matches)
 */
#ifndef VIZ__H
#define VIZ__H

typedef struct match_node_s {
    match_t *m;
    struct match_node_s *next;
} match_node_t;

__attribute__((nonnull))
void visualize_match(match_t *m);
__attribute__((nonnull))
void print_match(FILE *out, file_t *f, match_t *m, print_options_t options);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
