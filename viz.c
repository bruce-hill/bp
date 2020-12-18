/*
 * viz.c - Visualize matches.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "utils.h"
#include "viz.h"


static int match_height(match_t *m)
{
    int height = 0;
    for (match_t *c = m->child; c; c = c->nextsibling) {
        int childheight = match_height(c);
        if (childheight > height) height = childheight;
    }
    return 1 + height;
}

static void _visualize_matches(match_node_t *firstmatch, int depth, const char *text, size_t textlen)
{
    if (!firstmatch) return;

    const char *V = "│"; // Vertical bar
    const char *H = "─"; // Horizontal bar
    const char *color = (depth % 2 == 0) ? "34" : "33";

    match_t *viz = firstmatch->m;
    for (match_node_t *p = firstmatch; p; p = p->next)
        if (match_height(p->m) > match_height(viz))
            viz = p->m;
    const char *viz_type = viz->op->start;
    size_t viz_typelen = (size_t)(viz->op->end - viz->op->start);

    printf("\033[%ldG\033[%s;1m", 2*textlen+3, color);
    for (size_t i = 0; i < viz_typelen; i++) {
        switch (viz_type[i]) {
            case '\n': printf("↵"); break;
            default: printf("%c", viz_type[i]); break;
        }
    }
    printf("\033[0m");

    match_node_t *children = NULL;
    match_node_t **nextchild = &children;

#define RIGHT_TYPE(m) (m->m->op->end == m->m->op->start + viz_typelen && strncmp(m->m->op->start, viz_type, viz_typelen) == 0)
    // Print nonzero-width first:
    for (match_node_t *m = firstmatch; m; m = m->next) {
        //tree_text = byteslice(text, tree['start'], tree['end']).replace('\n', '↵')
        if (RIGHT_TYPE(m)) {
            //if (m->m->op->op != VM_REF) {
                for (match_t *c = m->m->child; c; c = c->nextsibling) {
                    *nextchild = new(match_node_t);
                    (*nextchild)->m = c;
                    nextchild = &((*nextchild)->next);
                }
            //}
            if (m->m->end == m->m->start) continue;
            printf("\033[%ldG\033[0;2m%s\033[0;7;%sm", 1+2*(m->m->start - text), V, color);
            for (const char *c = m->m->start; c < m->m->end; ++c) {
                // TODO: newline
                if (c > m->m->start) printf(" ");
                // TODO: utf8
                //while ((*c & 0xC0) != 0x80) printf("%c", *(c++));
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
        free(c);
    }
}

static void _visualize_patterns(match_t *m)
{
    if (m->op->op == VM_REF && streq(m->op->args.s, "pattern")) {
        m = m->child;
        match_node_t first = {.m = m};
        _visualize_matches(&first, 0, m->start, (size_t)(m->end - m->start));
    } else {
        for (match_t *c = m->child; c; c = c->nextsibling)
            _visualize_patterns(c);
    }
}

void visualize_match(match_t *m)
{
    printf("\033[?7l");
    //match_node_t first = {.m = m};
    //_visualize_matches(&first, 0, m->start, (m->end - m->start));
    _visualize_patterns(m);
    printf("\033[?7h");
}
