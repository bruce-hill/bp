//
// grammar.c - Code for defining grammars (sets of rules)
//

#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "file_loader.h"
#include "grammar.h"
#include "utils.h"

//
// Return a new list of definitions with one added to the front
//
def_t *with_def(def_t *defs, file_t *f, const char *name, vm_op_t *op)
{
    def_t *def = new(def_t);
    def->next = defs;
    def->file = f;
    def->name = name;
    def->op = op;
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
        check(src > name, "Invalid name for definition: %s", name);
        name = strndup(name, (size_t)(src-name));
        check(matchchar(&src, ':'), "Expected ':' in definition");
        vm_op_t *op = bp_pattern(f, src);
        if (op == NULL) break;
        defs = with_def(defs, f, name, op);
        src = op->end;
        src = after_spaces(src);
        if (matchchar(&src, ';'))
            src = after_spaces(src);
    }
    if (src < f->end) {
        fprint_line(stderr, f, src, NULL, "Invalid BP pattern");
        _exit(1);
    }
    return defs;
}

//
// Look up a backreference or grammar definition by name
//
vm_op_t *lookup(def_t *defs, const char *name)
{
    for ( ; defs; defs = defs->next) {
        if (streq(defs->name, name))
            return defs->op;
    }
    return NULL;
}

//
// Push a backreference onto the backreference stack
//
static def_t *with_backref(def_t *defs, file_t *f, const char *name, match_t *m)
{
    vm_op_t *op = new(vm_op_t);
    op->type = VM_BACKREF;
    op->start = m->start;
    op->end = m->end;
    op->len = -1; // TODO: maybe calculate this? (nontrivial because of replacements)
    op->args.backref = m;
    return with_def(defs, f, name, op);
}

//
// Push all the backreferences contained in a match onto the backreference stack
//
def_t *with_backrefs(def_t *defs, file_t *f, match_t *m)
{
    if (m->op->type != VM_REF) {
        if (m->op->type == VM_CAPTURE && m->op->args.capture.name)
            defs = with_backref(defs, f, m->op->args.capture.name, m->child);
        if (m->child) defs = with_backrefs(defs, f, m->child);
        if (m->nextsibling) defs = with_backrefs(defs, f, m->nextsibling);
    }
    return defs;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
