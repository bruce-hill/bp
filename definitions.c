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

//
// Return a new list of definitions with one added to the front
//
def_t *with_def(def_t *defs, size_t namelen, const char *name, pat_t *pat)
{
    def_t *def = new(def_t);
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
    const char *src = f->contents;
    src = after_spaces(src);
    while (src < f->end) {
        const char *name = src;
        src = after_name(name);
        if (src <= name)
            file_err(f, name, src, "Invalid name for definition: %s", name);
        size_t namelen = (size_t)(src - name);
        if (!matchchar(&src, ':'))
            errx(EXIT_FAILURE, "Expected ':' in definition");
        pat_t *pat = bp_pattern(f, src);
        if (pat == NULL) break;
        defs = with_def(defs, namelen, name, pat);
        src = pat->end;
        src = after_spaces(src);
        if (matchchar(&src, ';'))
            src = after_spaces(src);
    }
    if (src < f->end)
        file_err(f, src, NULL, "Invalid BP pattern");
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
// Push a backreference onto the backreference stack
//
def_t *with_backref(def_t *defs, file_t *f, size_t namelen, const char *name, match_t *m)
{
    pat_t *backref = new_pat(f, m->start, m->end, BP_BACKREF);
    backref->len = -1; // TODO: maybe calculate this? (nontrivial because of replacements)
    backref->args.backref = m;
    return with_def(defs, namelen, name, backref);
}

//
// Free all the given definitions up till (but not including) `stop`
//
void free_defs(def_t **defs, def_t *stop)
{
    while (*defs != stop && *defs != NULL) {
        def_t *next = (*defs)->next;
        (*defs)->next = NULL;
        free(*defs);
        (*defs) = next;
    }
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
