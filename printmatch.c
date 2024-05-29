//
// printmatch.c - Debug visualization of pattern matches.
//

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "match.h"
#include "printmatch.h"
#include "utils.h"

typedef struct match_node_s {
    bp_match_t *m;
    struct match_node_s *next;
} match_node_t;

__attribute__((nonnull, pure))
static int height_of_match(bp_match_t *m);
__attribute__((nonnull))
static void _explain_matches(match_node_t *firstmatch, int depth, const char *text, size_t textlen);

//
// Return the height of a match object (i.e. the number of descendents of the
// structure).
//
static int height_of_match(bp_match_t *m)
{
    int height = 0;
    for (int i = 0; m->children && m->children[i]; i++) {
        bp_match_t *child = m->children[i];
        int childheight = height_of_match(child);
        if (childheight > height) height = childheight;
    }
    return 1 + height;
}

//
// Print a visual explanation for the as-yet-unprinted matches provided.
//
static void _explain_matches(match_node_t *firstmatch, int depth, const char *text, size_t textlen)
{
    const char *V = "│"; // Vertical bar
    const char *H = "─"; // Horizontal bar
    const char *color = (depth % 2 == 0) ? "34" : "33";

    bp_match_t *viz = firstmatch->m;
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
        printf("\033[%zuG\033[0;2m\"\033[%s;1m", 2*textlen+3, color);
    else
        printf("\033[%zuG\033[%s;1m", 2*textlen+3, color);

    for (size_t i = 0; i < viz_typelen; i++) {
        switch (viz_type[i]) {
        case '\n': printf("↵"); break;
        case '\t': printf("⇥"); break;
        default: printf("%c", viz_type[i]); break;
        }
    }

    if (viz_type >= text && viz_type <= &text[textlen])
        printf("\033[0;2m\"");

    printf("\033[m");

    match_node_t *children = NULL;
    match_node_t **nextchild = &children;

#define RIGHT_TYPE(m) (m->m->pat->end == m->m->pat->start + viz_typelen && strncmp(m->m->pat->start, viz_type, viz_typelen) == 0)
    // Print nonzero-width first:
    for (match_node_t *m = firstmatch; m; m = m->next) {
        if (RIGHT_TYPE(m)) {
            // Instead of printing each subchain on its own line, flatten them all out at once:
            if (m->m->pat->type == BP_CHAIN) {
                bp_match_t *tmp = m->m;
                while (tmp->pat->type == BP_CHAIN) {
                    *nextchild = new(match_node_t);
                    (*nextchild)->m = tmp->children[0];
                    nextchild = &((*nextchild)->next);
                    tmp = tmp->children[1];
                }
                *nextchild = new(match_node_t);
                (*nextchild)->m = tmp;
                nextchild = &((*nextchild)->next);
            } else {
                for (int i = 0; m->m->children && m->m->children[i]; i++) {
                    *nextchild = new(match_node_t);
                    (*nextchild)->m = m->m->children[i];
                    nextchild = &((*nextchild)->next);
                }
            }
            if (m->m->end == m->m->start) continue;
            printf("\033[%zdG\033[0;2m%s\033[0;7;%sm", 1+2*(m->m->start - text), V, color);
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
            printf("\033[0;2m%s\033[m", V);
        } else {
            *nextchild = new(match_node_t);
            (*nextchild)->m = m->m;
            nextchild = &((*nextchild)->next);
            printf("\033[%zdG\033[0;2m%s", 1+2*(m->m->start - text), V);
            for (ssize_t i = (ssize_t)(2*(m->m->end - m->m->start)-1); i > 0; i--)
                printf(" ");
            if (m->m->end > m->m->start)
                printf("\033[0;2m%s", V);
            printf("\033[m");
        }
    }

    // Print stars for zero-width:
    for (match_node_t *m = firstmatch; m; m = m->next) {
        if (m->m->end > m->m->start) continue;
        if (RIGHT_TYPE(m)) {
            printf("\033[%zdG\033[7;%sm▒\033[m", 1+2*(m->m->start - text), color);
        } else {
            printf("\033[%zdG\033[0;2m%s\033[m", 1+2*(m->m->start - text), V);
        }
    }

    printf("\n");

    for (match_node_t *m = firstmatch; m; m = m->next) {
        if (m->m->end == m->m->start) {
            if (!RIGHT_TYPE(m))
                printf("\033[%zdG\033[0;2m%s", 1 + 2*(m->m->start - text), V);
        } else {
            const char *l = "└";
            const char *r = "┘";
            for (match_node_t *c = children; c; c = c->next) {
                if (c->m->start == m->m->start || c->m->end == m->m->start) l = V;
                if (c->m->start == m->m->end   || c->m->end == m->m->end) r = V;
            }
            printf("\033[%zdG\033[0;2m%s", 1 + 2*(m->m->start - text), l);
            const char *h = RIGHT_TYPE(m) ? H : " ";
            for (ssize_t n = (ssize_t)(2*(m->m->end - m->m->start) - 1); n > 0; n--)
                printf("%s", h);
            printf("%s\033[m", r);
        }
    }
#undef RIGHT_TYPE

    printf("\n");

    if (children)
        _explain_matches(children, depth+1, text, textlen);

    for (match_node_t *c = children, *next = NULL; c; c = next) {
        next = c->next;
        delete(&c);
    }
}

//
// Print a visualization of a match object.
//
public void explain_match(bp_match_t *m)
{
    printf("\033[?7l"); // Disable line wrapping
    match_node_t first = {.m = m};
    _explain_matches(&first, 0, m->start, (size_t)(m->end - m->start));
    printf("\033[?7h"); // Re-enable line wrapping
}

static inline int fputc_safe(FILE *out, char c, print_options_t *opts)
{
    int printed = fputc(c, out);
    if (c == '\n' && opts && opts->on_nl) {
        opts->on_nl(out);
        if (opts->replace_color) printed += fprintf(out, "%s", opts->replace_color);
    }
    return printed;
}

public int fprint_match(FILE *out, const char *file_start, bp_match_t *m, print_options_t *opts)
{
    int printed = 0;
    if (m->pat->type == BP_REPLACE) {
        auto rep = When(m->pat, BP_REPLACE);
        const char *text = rep->text;
        const char *end = &text[rep->len];
        if (opts && opts->replace_color) printed += fprintf(out, "%s", opts->replace_color);

        // TODO: clean up the line numbering code
        for (const char *r = text; r < end; ) {
            // Capture substitution
            if (*r == '@' && r+1 < end && r[1] != '@') {
                const char *next = r+1;
                // Retrieve the capture value:
                bp_match_t *cap = NULL;
                if (isdigit(*next)) {
                    int n = (int)strtol(next, (char**)&next, 10);
                    cap = get_numbered_capture(m->children[0], n);
                } else {
                    const char *name = next, *name_end = after_name(next, end);
                    if (name_end) {
                        cap = get_named_capture(m->children[0], name, (size_t)(name_end - name));
                        next = name_end;
                        if (next < m->end && *next == ';') ++next;
                    }
                }

                if (cap != NULL) {
                    printed += fprint_match(out, file_start, cap, opts);
                    if (opts && opts->replace_color) printed += fprintf(out, "%s", opts->replace_color);
                    r = next;
                    continue;
                }
            }

            if (*r == '\\') {
                ++r;
                if (*r == 'N') { // \N (nodent)
                    ++r;
                    // Mildly hacky: nodents here are based on the *first line*
                    // of the match. If the match spans multiple lines, or if
                    // the replacement text contains newlines, this may get weird.
                    const char *line_start = m->start;
                    while (line_start > file_start && line_start[-1] != '\n') --line_start;
                    printed += fputc_safe(out, '\n', opts);
                    for (const char *p = line_start; p < m->start && (*p == ' ' || *p == '\t'); ++p)
                        printed += fputc(*p, out);
                    continue;
                }
                printed += fputc_safe(out, unescapechar(r, &r, end), opts);
            } else {
                printed += fputc_safe(out, *r, opts);
                ++r;
            }
        }
    } else {
        if (opts && opts->match_color) printed += fprintf(out, "%s", opts->match_color);
        const char *prev = m->start;
        for (int i = 0; m->children && m->children[i]; i++) {
            bp_match_t *child = m->children[i];
            // Skip children from e.g. zero-width matches like >@foo
            if (!(prev <= child->start && child->start <= m->end &&
                  prev <= child->end && child->end <= m->end))
                continue;
            if (child->start > prev) {
                if (opts && opts->fprint_between) printed += opts->fprint_between(out, prev, child->start, opts->match_color);
                else printed += fwrite(prev, sizeof(char), (size_t)(child->start - prev), out);
            }
            printed += fprint_match(out, file_start, child, opts);
            if (opts && opts->match_color) printed += fprintf(out, "%s", opts->match_color);
            prev = child->end;
        }
        if (m->end > prev) {
            if (opts && opts->fprint_between) printed += opts->fprint_between(out, prev, m->end, opts->match_color);
            else printed += fwrite(prev, sizeof(char), (size_t)(m->end - prev), out);
        }
    }
    return printed;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
