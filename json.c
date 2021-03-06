//
// json.c - Code for printing JSON output of matches.
//

#include <stdio.h>

#include "json.h"

__attribute__((nonnull))
static int _json_match(const char *text, match_t *m, int comma, bool verbose);

//
// Helper function for json_match().
// `comma` is used to track whether a comma will need to be printed before the
// next object or not.
//
static int _json_match(const char *text, match_t *m, int comma, bool verbose)
{
    if (!verbose) {
        if (m->pat->type != BP_REF && m->pat->type != BP_ERROR) {
            for (match_t *child = m->child; child; child = child->nextsibling) {
                comma |= _json_match(text, child, comma, verbose);
            }
            return comma;
        }
    }

    if (comma) printf(",\n");
    comma = 0;
    printf("{\"rule\":\"");
    for (const char *c = m->pat->start; c < m->pat->end; c++) {
        switch (*c) {
            case '"': printf("\\\""); break;
            case '\\': printf("\\\\"); break;
            case '\t': printf("\\t"); break;
            case '\n': printf("↵"); break;
            default: printf("%c", *c); break;
        }
    }
    printf("\",\"start\":%ld,\"end\":%ld,\"children\":[",
            m->start - text, m->end - text);
    for (match_t *child = m->child; child; child = child->nextsibling) {
        comma |= _json_match(text, child, comma, verbose);
    }
    printf("]}");
    return 1;
}

//
// Print a match object as a JSON object.
//
void json_match(const char *text, match_t *m, bool verbose)
{
    (void)_json_match(text, m, 0, verbose);
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
