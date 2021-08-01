//
// definitions.c - Code for defining named pattern rules
//

#include <err.h>
#include <stdlib.h>
#include <string.h>

#include "definitions.h"
#include "files.h"
#include "pattern.h"
#include "utils.h"

static size_t next_id = 0;

//
// Return a new list of definitions with one added to the front
//
def_t *with_def(def_t *defs, size_t namelen, const char *name, pat_t *pat)
{
    def_t *def = new(def_t);
    def->id = next_id++;
    def->next = defs;
    def->namelen = namelen;
    def->name = name;
    def->pat = pat;
    return def;
}

//
// Load the given grammar (semicolon-separated definitions)
// and return the first rule defined.
//
def_t *load_grammar(def_t *defs, file_t *f)
{
    const char *str = after_spaces(f->start);
    while (*str == '\r' || *str == '\n') str = after_spaces(++str);
    pat_t *pat = bp_pattern(f, str);
    if (!pat) file_err(f, str, f->end, "Could not parse this file");
    if (pat->end < f->end) file_err(f, pat->end, f->end, "Could not parse this part of the file");
    for (pat_t *p = pat; p && p->type == BP_DEFINITION; p = p->args.def.pat) {
        // printf("Def '%.*s': %.*s\n", (int)p->args.def.namelen, p->args.def.name,
        //        (int)(p->args.def.def->end - p->args.def.def->start), p->args.def.def->start);
        defs = with_def(defs, p->args.def.namelen, p->args.def.name, p->args.def.def);
    }
    return defs;
}

//
// Look up a backreference or grammar definition by name
//
def_t *lookup(def_t *defs, size_t namelen, const char *name)
{
    for ( ; defs; defs = defs->next) {
        if (namelen == defs->namelen && strncmp(defs->name, name, namelen) == 0)
            return defs;
    }
    return NULL;
}

//
// Free all the given definitions up till (but not including) `stop`
//
def_t *free_defs(def_t *defs, def_t *stop)
{
    while (defs != stop && defs != NULL) {
        def_t *next = defs->next;
        defs->next = NULL;
        free(defs);
        defs = next;
    }
    return defs;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
