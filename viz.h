/*
 * Header file for viz.c (visualizing matches)
 */

typedef struct match_node_s {
    match_t *m;
    struct match_node_s *next;
} match_node_t;

void visualize_match(match_t *m);
