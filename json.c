/*
 * json.c - Code for printing JSON output of matches.
 */
#include "types.h"

/*
 * Print a match as JSON
 */
static int _json_match(FILE *f, const char *text, match_t *m, int comma, int verbose)
{
    if (!verbose) {
        if (m->op->op != VM_REF) {
            for (match_t *child = m->child; child; child = child->nextsibling) {
                comma |= _json_match(f, text, child, comma, verbose);
            }
            return comma;
        }
    }

    if (comma) fprintf(f, ",\n");
    comma = 0;
    fprintf(f, "{\"rule\":\"");
    for (const char *c = m->op->start; c < m->op->end; c++) {
        switch (*c) {
            case '"': fprintf(f, "\\\""); break;
            case '\\': fprintf(f, "\\\\"); break;
            case '\t': fprintf(f, "\\t"); break;
            case '\n': fprintf(f, "↵"); break;
            default: fprintf(f, "%c", *c); break;
        }
    }
    fprintf(f, "\",\"start\":%ld,\"end\":%ld,\"children\":[",
            m->start - text, m->end - text);
    for (match_t *child = m->child; child; child = child->nextsibling) {
        comma |= _json_match(f, text, child, comma, verbose);
    }
    fprintf(f, "]}");
    return 1;
}

void json_match(FILE *f, const char *text, match_t *m, int verbose)
{
    _json_match(f, text, m, 0, verbose);
}
