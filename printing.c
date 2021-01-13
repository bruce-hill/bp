/*
 * printing.c - Code for printing and visualizing matches.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "printing.h"
#include "types.h"
#include "utils.h"
#include "vm.h"

typedef struct match_node_s {
    match_t *m;
    struct match_node_s *next;
} match_node_t;

typedef struct {
    size_t line, printed_line;
    const char *color;
} print_state_t;

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
    // This is a heuristic: print matches first if they have more submatches.
    // In general, this helps reduce the height of the final output by allowing
    // for more rows that show the same rule matching in multiple places.
    // TODO: there may be a better heuristic that optimizes for this factor
    // while also printing earlier matches first when it doesn't affect overall
    // output height.
    for (match_node_t *p = firstmatch; p; p = p->next)
        if (match_height(p->m) > match_height(viz))
            viz = p->m;
    const char *viz_type = viz->op->start;
    size_t viz_typelen = (size_t)(viz->op->end - viz->op->start);

    // Backrefs use added dim quote marks to indicate that the pattern is a
    // literal string being matched. (Backrefs have start/end inside the text
    // input, instead of something the user typed in)
    if (viz_type >= text && viz_type <= &text[textlen])
        printf("\033[%ldG\033[0;2m\"\033[%s;1m", 2*textlen+3, color);
    else
        printf("\033[%ldG\033[%s;1m", 2*textlen+3, color);

    for (size_t i = 0; i < viz_typelen; i++) {
        switch (viz_type[i]) {
            case '\n': printf("↵"); break;
            default: printf("%c", viz_type[i]); break;
        }
    }

    if (viz_type >= text && viz_type <= &text[textlen])
        printf("\033[0;2m\"");

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
        xfree(&c);
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

static void print_line_number(FILE *out, print_state_t *state, print_options_t options)
{
    state->printed_line = state->line;
    if (!(options & PRINT_LINE_NUMBERS)) return;
    if (options & PRINT_COLOR)
        fprintf(out, "\033[0;2m% 5ld\033(0\x78\033(B%s", state->line, state->color);
    else
        fprintf(out, "% 5ld|", state->line);
}

/*
 * Print a match with replacements and highlighting.
 */
static void _print_match(FILE *out, file_t *f, match_t *m, print_state_t *state, print_options_t options)
{
    static const char *hl = "\033[0;31;1m";
    const char *old_color = state->color;
    if (m->op->op == VM_HIDE) {
        // TODO: handle replacements?
        for (const char *p = m->start; p < m->end; p++) {
            if (*p == '\n') ++state->line;
        }
    } else if (m->op->op == VM_REPLACE) {
        if (options & PRINT_COLOR && state->color != hl) {
            state->color = hl;
            fprintf(out, "%s", state->color);
        }
        const char *text = m->op->args.replace.text;
        const char *end = &text[m->op->args.replace.len];
        for (const char *r = text; r < end; ) {
            if (*r == '@' && r[1] && r[1] != '@') {
                ++r;
                match_t *cap = get_capture(m, &r);
                if (cap != NULL) {
                    _print_match(out, f, cap, state, options);
                    continue;
                } else {
                    --r;
                }
            }

            if (state->printed_line != state->line)
                print_line_number(out, state, options);

            if (*r == '\\') {
                ++r;
                unsigned char c = unescapechar(r, &r);
                fputc(c, out);
                if (c == '\n') ++state->line;
                continue;
            } else if (*r == '\n') {
                fputc('\n', out);
                ++state->line;
                ++r;
                continue;
            } else {
                fputc(*r, out);
                ++r;
                continue;
            }
        }
    } else {
        if (m->op->op == VM_CAPTURE) {
            if (options & PRINT_COLOR && state->color != hl) {
                state->color = hl;
                fprintf(out, "%s", state->color);
            }
        }

        const char *prev = m->start;
        for (match_t *child = m->child; child; child = child->nextsibling) {
            // Skip children from e.g. zero-width matches like >@foo
            if (!(prev <= child->start && child->start <= m->end &&
                  prev <= child->end && child->end <= m->end))
                continue;
            if (child->start > prev) {
                for (const char *p = prev; p < child->start; ++p) {
                    if (state->printed_line != state->line)
                        print_line_number(out, state, options);
                    fputc(*p, out);
                    if (*p == '\n') ++state->line;
                }
            }
            _print_match(out, f, child, state, options);
            prev = child->end;
        }
        if (m->end > prev) {
            for (const char *p = prev; p < m->end; ++p) {
                if (state->printed_line != state->line)
                    print_line_number(out, state, options);
                fputc(*p, out);
                if (*p == '\n') ++state->line;
            }
        }
    }
    if (options & PRINT_COLOR && old_color != state->color) {
        fprintf(out, "%s", old_color);
        state->color = old_color;
    }
}

void print_match(FILE *out, file_t *f, match_t *m, print_options_t options)
{
    print_state_t state = {.line = 1, .color = "\033[0m"};
    _print_match(out, f, m, &state, options);
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
