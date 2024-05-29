//
// json.c - Code for printing JSON output of matches.
//

#include <stdio.h>

#include "json.h"
#include "utils.h"

__attribute__((nonnull))
static int _json_match(const char *text, bp_match_t *m, int comma, bool verbose);

//
// Helper function for json_match().
// `comma` is used to track whether a comma will need to be printed before the
// next object or not.
//
static int _json_match(const char *text, bp_match_t *m, int comma, bool verbose)
{
    if (!verbose && m->pat->type != BP_TAGGED) {
        if (m->children) {
            for (int i = 0; m->children && m->children[i]; i++)
                comma |= _json_match(text, m->children[i], comma, verbose);
        }
        return comma;
    }

    if (comma) printf(",\n");
    comma = 0;
    printf("{");
    if (m->pat->type == BP_TAGGED) {
        printf("\"tag\":\"%.*s\"", (int)When(m->pat, BP_TAGGED)->namelen, When(m->pat, BP_TAGGED)->name);
        comma = 1;
    }
    if (verbose) {
        if (comma) printf(",");
        printf("\"rule\":\"");
        for (const char *c = m->pat->start; c < m->pat->end; c++) {
            switch (*c) {
            case '"': printf("\\\""); break;
            case '\\': printf("\\\\"); break;
            case '\t': printf("\\t"); break;
            case '\n': printf("↵"); break;
            default: printf("%c", *c); break;
            }
        }
        printf("\",");
        printf("\"range\":[%zd,%zd]", m->start - text, m->end - text);
        comma = 1;
    } else {
        if (comma) printf(",");
        printf("\"text\":\"");
        for (const char *c = m->start; c < m->end; c++) {
            switch (*c) {
            case '"': printf("\\\""); break;
            case '\\': printf("\\\\"); break;
            case '\t': printf("\\t"); break;
            case '\n': printf("↵"); break;
            default: printf("%c", *c); break;
            }
        }
        printf("\"");
        comma = 1;
    }

    if (m->children) {
        if (comma) printf(",");
        printf("\"children\":[");
        comma = 0;
        for (int i = 0; m->children && m->children[i]; i++)
            comma |= _json_match(text, m->children[i], comma, verbose);
        printf("]");
    }
    printf("}");
    return 1;
}

//
// Print a match object as a JSON object.
//
public void json_match(const char *text, bp_match_t *m, bool verbose)
{
    (void)_json_match(text, m, 0, verbose);
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
