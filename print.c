//
// print.c - Code for printing and visualizing matches.
//

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "files.h"
#include "match.h"
#include "print.h"
#include "types.h"
#include "utils.h"

static const char *color_match = "\033[0;31;1m";
static const char *color_replace = "\033[0;34;1m";
static const char *color_normal = "\033[0m";
static const char *current_color = NULL;

//
// Print a line number, if it needs to be printed.
// line number of 0 means "just print an empty space for the number"
//
__attribute__((nonnull(1,2)))
static inline void print_line_number(FILE *out, printer_t *pr, size_t line_number, const char *color)
{
    if (!pr->needs_line_number) return;
    if (pr->print_line_numbers) {
        if (line_number == 0) {
            if (color) fprintf(out, "\033[0;2m     \033(0\x78\033(B%s", color);
            else fprintf(out, "     |");
        } else {
            if (color) fprintf(out, "\033[0;2m%5lu\033(0\x78\033(B%s", line_number, color);
            else fprintf(out, "%5lu|", line_number);
        }
        current_color = color;
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
        if (color && color != current_color) {
            fprintf(out, "%s", color);
            current_color = color;
        }
        for (const char *c = start; c < eol; c++)
            fputc(*c, out);
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
    if (pr->context_before == ALL_CONTEXT) {
        return pr->pos;
    } else if (pr->context_before >= 0) {
        size_t n = get_line_number(pr->file, pos);
        if (n >= (size_t)(pr->context_before + 1))
            n -= (size_t)pr->context_before;
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
    if (pr->context_after == ALL_CONTEXT) {
        return pr->file->end;
    } else if (pr->context_after >= 0) {
        size_t n = get_line_number(pr->file, pos) + (size_t)pr->context_after;
        const char *eol = get_line(pr->file, n+1);
        return eol ? eol : pr->file->end;
    } else {
        return pos;
    }
}

//
// Print the text of a match (no context).
//
static void _print_match(FILE *out, printer_t *pr, match_t *m)
{
    pr->pos = m->start;
    if (m->pat->type == BP_REPLACE) {
        if (m->skip_replacement) {
            _print_match(out, pr, m->children[0]);
            return;
        }
        size_t line = get_line_number(pr->file, m->start);
        size_t line_end = get_line_number(pr->file, m->end);

        if (pr->use_color && current_color != color_replace) {
            fprintf(out, "%s", color_replace);
            current_color = color_replace;
        }
        const char *text = m->pat->args.replace.text;
        const char *end = &text[m->pat->args.replace.len];

        // TODO: clean up the line numbering code
        for (const char *r = text; r < end; ) {
            print_line_number(out, pr, line > line_end ? 0 : line, pr->use_color ? color_replace : NULL);

            // Capture substitution
            if (*r == '@' && r[1] && r[1] != '@') {
                ++r;
                match_t *cap = get_capture(m->children[0], &r);
                if (cap != NULL) {
                    _print_match(out, pr, cap);
                    if (pr->use_color && current_color != color_replace) {
                        fprintf(out, "%s", color_replace);
                        current_color = color_replace;
                    }
                    continue;
                } else {
                    --r;
                }
            }

            if (*r == '\\') {
                ++r;
                if (*r == 'N') { // \N (nodent)
                    ++r;
                    // Mildly hacky: nodents here are based on the *first line*
                    // of the match. If the match spans multiple lines, or if
                    // the replacement text contains newlines, this may get weird.
                    const char *line_start = get_line(
                        pr->file, get_line_number(pr->file, m->start));
                    char denter = line_start ? *line_start : '\t';
                    fputc('\n', out);
                    ++line;
                    pr->needs_line_number = 1;
                    print_line_number(out, pr, 0, pr->use_color ? color_replace : NULL);
                    if (denter == ' ' || denter == '\t') {
                        for (const char *p = line_start; p && *p == denter && p < m->start; ++p)
                            fputc(denter, out);
                    }
                    continue;
                }
                const char *start = r;
                char c = unescapechar(r, &r);
                if (r > start) {
                    (void)fputc(c, out);
                    if (c == '\n') {
                        ++line;
                        pr->needs_line_number = 1;
                    }
                } else (void)fputc('\\', out);
                continue;
            } else if (*r == '\n') {
                (void)fputc('\n', out);
                ++line;
                pr->needs_line_number = 1;
                ++r;
                continue;
            } else {
                (void)fputc(*r, out);
                ++r;
                continue;
            }
        }
        print_line_number(out, pr, line > line_end ? 0 : line, pr->use_color ? color_normal : NULL);
    } else {
        const char *prev = m->start;
        for (int i = 0; m->children && m->children[i]; i++) {
            match_t *child = m->children[i];
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
    current_color = color_normal;
    bool first = (pr->pos == NULL);
    if (first) { // First match printed:
        pr->pos = pr->file->start;
        pr->needs_line_number = 1;
    }
    if (m) {
        const char *before_m = context_before(pr, m->start);
        if (!first) {
            // When not printing context lines, print each match on its own
            // line instead of jamming them all together:
            if (pr->context_before == NO_CONTEXT && pr->context_after == NO_CONTEXT && (!pr->needs_line_number || !pr->print_line_numbers)) {
                fprintf(out, "\n");
                pr->needs_line_number = 1;
            }

            const char *after_last = context_after(pr, pr->pos);
            if (after_last >= before_m) {
                // Overlapping ranges:
                before_m = pr->pos;
            } else {
                // Non-overlapping ranges:
                print_between(out, pr, pr->pos, after_last, pr->use_color ? color_normal : NULL);
                if (pr->context_before > 0 || pr->context_after > 0)
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
        if (!pr->needs_line_number) fprintf(out, "\n");
    }
    if (pr->use_color) fprintf(out, "%s", color_normal);
}

//
// Print any errors that are present in the given match object to stderr and
// return the number of errors found.
//
int print_errors(file_t *f, match_t *m)
{
    int ret = 0;
    if (m->pat->type == BP_ERROR) {
        fprintf(stderr, "\033[31;1m");
        printer_t tmp = {.file = f, .context_before=NO_CONTEXT, .context_after=NO_CONTEXT}; // No bells and whistles
        print_match(stderr, &tmp, m); // Error message
        fprintf(stderr, "\033[0m\n");
        fprint_line(stderr, f, m->start, m->end, " ");
        return 1;
    }
    for (int i = 0; m->children && m->children[i]; i++)
        ret += print_errors(f, m->children[i]);
    return ret;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
