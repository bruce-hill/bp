/*
 * Header file for viz.c (visualizing matches)
 */
#ifndef VIZ__H
#define VIZ__H

typedef struct match_node_s {
    match_t *m;
    struct match_node_s *next;
} match_node_t;

void visualize_match(match_t *m);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
