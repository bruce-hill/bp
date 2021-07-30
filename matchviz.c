//
// debugviz.c - Debug visualization of pattern matches.
//

#include <stdio.h>
#include <string.h>

#include "matchviz.h"
#include "types.h"
#include "utils.h"

typedef struct match_node_s {
    match_t *m;
    struct match_node_s *next;
} match_node_t;

__attribute__((nonnull, pure))
static int height_of_match(match_t *m);
__attribute__((nonnull))
static void _visualize_matches(match_node_t *firstmatch, int depth, const char *text, size_t textlen);

//
// Return the height of a match object (i.e. the number of descendents of the
// structure).
//
static int height_of_match(match_t *m)
{
    int height = 0;
    for (int i = 0; m->children && m->children[i]; i++) {
        match_t *child = m->children[i];
        int childheight = height_of_match(child);
        if (childheight > height) height = childheight;
    }
    return 1 + height;
}

//
// Print a visual explanation for the as-yet-unprinted matches provided.
//
static void _visualize_matches(match_node_t *firstmatch, int depth, const char *text, size_t textlen)
{
    const char *V = "│"; // Vertical bar
    const char *H = "─"; // Horizontal bar
    const char *color = (depth % 2 == 0) ? "34" : "33";

    match_t *viz = firstmatch->m;
    // This is a heuristic: print matches first if they have more submatches.
    // In general, this helps reduce the height of the final output by allowing
    // for more rows that show the same rule matching in multiple places.
    // TODO: there may be a better heuristic that optimizes for this factor
    // while also printing earlier matches first when it doesn't affect overall
    // output height.
    for (match_node_t *p = firstmatch; p; p = p->next)
        if (height_of_match(p->m) > height_of_match(viz))
            viz = p->m;
    const char *viz_type = viz->pat->start;
    size_t viz_typelen = (size_t)(viz->pat->end - viz->pat->start);

    // Backrefs use added dim quote marks to indicate that the pattern is a
    // literal string being matched. (Backrefs have start/end inside the text
    // input, instead of something the user typed in)
    if (viz_type >= text && viz_type <= &text[textlen])
        printf("\033[%luG\033[0;2m\"\033[%s;1m", 2*textlen+3, color);
    else
        printf("\033[%luG\033[%s;1m", 2*textlen+3, color);

    for (size_t i = 0; i < viz_typelen; i++) {
        switch (viz_type[i]) {
            case '\n': printf("↵"); break;
            case '\t': printf("⇥"); break;
            default: printf("%c", viz_type[i]); break;
        }
    }

    if (viz_type >= text && viz_type <= &text[textlen])
        printf("\033[0;2m\"");

    printf("\033[0m");

    match_node_t *children = NULL;
    match_node_t **nextchild = &children;

#define RIGHT_TYPE(m) (m->m->pat->end == m->m->pat->start + viz_typelen && strncmp(m->m->pat->start, viz_type, viz_typelen) == 0)
    // Print nonzero-width first:
    for (match_node_t *m = firstmatch; m; m = m->next) {
        if (RIGHT_TYPE(m)) {
            for (int i = 0; m->m->children && m->m->children[i]; i++) {
                *nextchild = new(match_node_t);
                (*nextchild)->m = m->m->children[i];
                nextchild = &((*nextchild)->next);
            }
            if (m->m->end == m->m->start) continue;
            printf("\033[%ldG\033[0;2m%s\033[0;7;%sm", 1+2*(m->m->start - text), V, color);
            for (const char *c = m->m->start; c < m->m->end; ++c) {
                // TODO: newline
                if (c > m->m->start) printf(" ");
                // TODO: utf8
                //while ((*c & 0xC0) != 0x80) printf("%c", *(c++));
                if (*c == '\n')
                    printf("↵");
                else if (*c == '\t')
                    printf("⇥");
                else
                    printf("%c", *c);
            }
            printf("\033[0;2m%s\033[0m", V);
        } else {
            *nextchild = new(match_node_t);
            (*nextchild)->m = m->m;
            nextchild = &((*nextchild)->next);
            printf("\033[%ldG\033[0;2m%s", 1+2*(m->m->start - text), V);
            for (ssize_t i = (ssize_t)(2*(m->m->end - m->m->start)-1); i > 0; i--)
                printf(" ");
            if (m->m->end > m->m->start)
                printf("\033[0;2m%s", V);
            printf("\033[0m");
        }
    }

    // Print stars for zero-width:
    for (match_node_t *m = firstmatch; m; m = m->next) {
        if (m->m->end > m->m->start) continue;
        if (RIGHT_TYPE(m)) {
            printf("\033[%ldG\033[7;%sm▒\033[0m", 1+2*(m->m->start - text), color);
        } else {
            printf("\033[%ldG\033[0;2m%s\033[0m", 1+2*(m->m->start - text), V);
        }
    }

    printf("\n");

    for (match_node_t *m = firstmatch; m; m = m->next) {
        if (m->m->end == m->m->start) {
            if (!RIGHT_TYPE(m))
                printf("\033[%ldG\033[0;2m%s", 1 + 2*(m->m->start - text), V);
        } else {
            const char *l = "└";
            const char *r = "┘";
            for (match_node_t *c = children; c; c = c->next) {
                if (c->m->start == m->m->start || c->m->end == m->m->start) l = V;
                if (c->m->start == m->m->end   || c->m->end == m->m->end) r = V;
            }
            printf("\033[%ldG\033[0;2m%s", 1 + 2*(m->m->start - text), l);
            const char *h = RIGHT_TYPE(m) ? H : " ";
            for (ssize_t n = (ssize_t)(2*(m->m->end - m->m->start) - 1); n > 0; n--)
                printf("%s", h);
            printf("%s\033[0m", r);
        }
    }
#undef RIGHT_TYPE

    printf("\n");

    if (children)
        _visualize_matches(children, depth+1, text, textlen);

    for (match_node_t *c = children, *next = NULL; c; c = next) {
        next = c->next;
        delete(&c);
    }
}

//
// Print a visualization of a match object.
//
void visualize_match(match_t *m)
{
    printf("\033[?7l"); // Disable line wrapping
    match_node_t first = {.m = m};
    _visualize_matches(&first, 0, m->start, (size_t)(m->end - m->start));
    printf("\033[?7h"); // Re-enable line wrapping
}
