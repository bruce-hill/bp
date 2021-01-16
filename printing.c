//
// printing.c - Code for printing and visualizing matches.
//

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

static const char *color_match = "\033[0;31;1m";
static const char *color_replace = "\033[0;34;1m";
static const char *color_normal = "\033[0m";

__attribute__((nonnull, pure))
static int height_of_match(match_t *m);
__attribute__((nonnull))
static void _visualize_matches(match_node_t *firstmatch, int depth, const char *text, size_t textlen);
__attribute__((nonnull(1,2)))
static inline void print_line_number(FILE *out, printer_t *pr, size_t line_number, const char *color);

//
// Return the height of a match object (i.e. the number of descendents of the
// structure).
//
static int height_of_match(match_t *m)
{
    int height = 0;
    for (match_t *c = m->child; c; c = c->nextsibling) {
        int childheight = height_of_match(c);
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

#define RIGHT_TYPE(m) (m->m->pat->end == m->m->pat->start + viz_typelen && strncmp(m->m->pat->start, viz_type, viz_typelen) == 0)
    // Print nonzero-width first:
    for (match_node_t *m = firstmatch; m; m = m->next) {
        if (RIGHT_TYPE(m)) {
            for (match_t *c = m->m->child; c; c = c->nextsibling) {
                *nextchild = new(match_node_t);
                (*nextchild)->m = c;
                nextchild = &((*nextchild)->next);
            }
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

//
// Print a line number, if it needs to be printed.
// line number of 0 means "just print an empty space for the number"
//
__attribute__((nonnull(1,2)))
static inline void print_line_number(FILE *out, printer_t *pr, size_t line_number, const char *color)
{
    if (!pr->print_line_numbers) return;
    if (!pr->needs_line_number) return;
    if (line_number == 0) {
        if (color) fprintf(out, "\033[0;2m     \033(0\x78\033(B%s", color);
        else fprintf(out, "     |");
    } else {
        if (color) fprintf(out, "\033[0;2m% 5ld\033(0\x78\033(B%s", line_number, color);
        else fprintf(out, "% 5ld|", line_number);
    }
    pr->needs_line_number = 0;
}

// 
// Print a range of text from a file, adding line numbers if necessary.
//
__attribute__((nonnull(1,2,3,4)))
static void print_between(FILE *out, printer_t *pr, const char *start, const char *end, const char *color)
{
    file_t *f = pr->file;
    while (start < end) {
        size_t line_num = get_line_number(f, start);
        print_line_number(out, pr, line_num, color);
        const char *eol = get_line(pr->file, line_num + 1);
        if (!eol || eol > end) eol = end;
        if (color) fprintf(out, "%s", color);
        fprintf(out, "%.*s", (int)(eol - start), start);
        if (eol[-1] == '\n')
            pr->needs_line_number = 1;
        start = eol;
    }
    pr->pos = end;
}

//
// Return a pointer to the first character of context information before `pos`,
// according to the context settings in `pr`
//
static const char *context_before(printer_t *pr, const char *pos)
{
    if (pr->context_lines == -1) {
        return pr->pos;
    } else if (pr->context_lines > 0) {
        size_t n = get_line_number(pr->file, pos);
        if (n >= (size_t)((pr->context_lines - 1) + 1))
            n -= (size_t)(pr->context_lines - 1);
        else
            n = 1;
        const char *sol = get_line(pr->file, n);
        if (sol == NULL || sol < pr->pos) sol = pr->pos;
        return sol;
    } else {
        return pos;
    }
}

//
// Return a pointer to the last character of context information after `pos`,
// according to the context settings in `pr`
//
static const char *context_after(printer_t *pr, const char *pos)
{
    if (pr->context_lines == -1) {
        return pr->file->end;
    } else if (pr->context_lines > 0) {
        size_t n = get_line_number(pr->file, pos) + (size_t)(pr->context_lines - 1);
        const char *eol = get_line(pr->file, n+1);
        return eol ? eol : pr->file->end;
    } else {
        return pos;
    }
}

//
// Print the text of a match (no context).
//
void _print_match(FILE *out, printer_t *pr, match_t *m)
{
    pr->pos = m->start;
    if (m->pat->type == VM_REPLACE) {
        if (m->skip_replacement) {
            _print_match(out, pr, m->child);
            return;
        }
        size_t line_start = get_line_number(pr->file, m->start);
        size_t line_end = get_line_number(pr->file, m->end);
        size_t line = line_start;

        if (pr->use_color) fprintf(out, "%s", color_replace);
        const char *text = m->pat->args.replace.text;
        const char *end = &text[m->pat->args.replace.len];

        // TODO: clean up the line numbering code
        for (const char *r = text; r < end; ) {
            print_line_number(out, pr, line > line_end ? 0 : line, pr->use_color ? color_replace : NULL);

            // Capture substitution
            if (*r == '@' && r[1] && r[1] != '@') {
                ++r;
                match_t *cap = get_capture(m, &r);
                if (cap != NULL) {
                    _print_match(out, pr, cap);
                    if (pr->use_color) fprintf(out, "%s", color_replace);
                    continue;
                } else {
                    --r;
                }
            }

            if (*r == '\\') {
                ++r;
                unsigned char c = unescapechar(r, &r);
                fputc(c, out);
                if (c == '\n') {
                    ++line;
                    pr->needs_line_number = 1;
                }
                continue;
            } else if (*r == '\n') {
                fputc('\n', out);
                ++line;
                pr->needs_line_number = 1;
                ++r;
                continue;
            } else {
                fputc(*r, out);
                ++r;
                continue;
            }
        }
        print_line_number(out, pr, line > line_end ? 0 : line, pr->use_color ? color_normal : NULL);
    } else {
        const char *prev = m->start;
        for (match_t *child = m->child; child; child = child->nextsibling) {
            // Skip children from e.g. zero-width matches like >@foo
            if (!(prev <= child->start && child->start <= m->end &&
                  prev <= child->end && child->end <= m->end))
                continue;
            if (child->start > prev)
                print_between(out, pr, prev, child->start, pr->use_color ? color_match : NULL);
            _print_match(out, pr, child);
            prev = child->end;
        }
        if (m->end > prev)
            print_between(out, pr, prev, m->end, pr->use_color ? color_match : NULL);
    }
    pr->pos = m->end;
}

//
// Print the text of a match and any context.
//
void print_match(FILE *out, printer_t *pr, match_t *m)
{
    int first = (pr->pos == NULL);
    if (first) { // First match printed:
        pr->pos = pr->file->contents;
        pr->needs_line_number = 1;
    }
    if (m) {
        const char *before_m = context_before(pr, m->start);
        if (!first) {
            const char *after_last = context_after(pr, pr->pos);
            if (after_last >= before_m) {
                // Overlapping ranges:
                before_m = pr->pos;
            } else {
                // Non-overlapping ranges:
                print_between(out, pr, pr->pos, after_last, pr->use_color ? color_normal : NULL);
                if (pr->context_lines > 1)
                    fprintf(out, "\n"); // Gap between chunks
            }
        }
        print_between(out, pr, before_m, m->start, pr->use_color ? color_normal : NULL);
        _print_match(out, pr, m);
    } else {
        // After the last match is printed, print the trailing context:
        const char *after_last = context_after(pr, pr->pos);
        print_between(out, pr, pr->pos, after_last, pr->use_color ? color_normal : NULL);
        // Guarantee trailing newline
        if (pr->pos > pr->file->contents && pr->pos[-1] != '\n') fprintf(out, "\n");
    }
    if (pr->use_color) fprintf(out, "%s", color_normal);
}

//
// Print any errors that are present in the given match object.
//
int print_errors(printer_t *pr, match_t *m)
{
    int ret = 0;
    if (m->pat->type == VM_CAPTURE && m->pat->args.capture.name && streq(m->pat->args.capture.name, "!")) {
        printf("\033[31;1m");
        print_match(stdout, pr, m);
        printf("\033[0m\n");
        fprint_line(stdout, pr->file, m->start, m->end, " ");
        return 1;
    }
    if (m->child) ret += print_errors(pr, m->child);
    if (m->nextsibling) ret += print_errors(pr, m->nextsibling);
    return ret;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
