/*
 * grammar.c - Code for defining grammars (sets of rules)
 */

#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "file_loader.h"
#include "grammar.h"
#include "utils.h"

/*
 * Add a definition to the grammar
 */
void add_def(grammar_t *g, file_t *f, const char *src, const char *name, vm_op_t *op)
{
    def_t *def = new(def_t);
    def->next = g->firstdef;
    def->file = f;
    def->source = src;
    def->name = name;
    def->op = op;
    g->firstdef = def;
}

/*
 * Load the given grammar (semicolon-separated definitions)
 * and return the first rule defined.
 */
vm_op_t *load_grammar(grammar_t *g, file_t *f)
{
    vm_op_t *ret = NULL;
    const char *src = f->contents;
    src = after_spaces(src);
    while (*src) {
        const char *name = src;
        const char *name_end = after_name(name);
        check(name_end > name, "Invalid name for definition: %s", name);
        name = strndup(name, (size_t)(name_end-name));
        src = after_spaces(name_end);
        check(matchchar(&src, ':'), "Expected ':' in definition");
        vm_op_t *op = bp_pattern(f, src);
        if (op == NULL) break;
        //check(op, "Couldn't load definition");
        add_def(g, f, src, name, op);
        if (ret == NULL) {
            ret = op;
        }
        src = op->end;
        src = after_spaces(src);
        if (*src && matchchar(&src, ';'))
            src = after_spaces(src);
    }
    if (src < f->end) {
        fprint_line(stderr, f, src, NULL, "Invalid BP pattern");
        _exit(1);
    }
    return ret;
}

/*
 * Look up a backreference or grammar definition by name
 */
vm_op_t *lookup(grammar_t *g, const char *name)
{
    for (backref_t *b = g->firstbackref; b; b = b->next) {
        if (streq(b->name, name))
            return b->op;
    }
    for (def_t *d = g->firstdef; d; d = d->next) {
        if (streq(d->name, name))
            return d->op;
    }
    return NULL;
}

/*
 * Push a backreference onto the backreference stack
 */
void push_backref(grammar_t *g, const char *name, match_t *capture)
{
    backref_t *backref = new(backref_t);
    backref->name = name;
    backref->capture = capture;
    vm_op_t *op = new(vm_op_t);
    op->op = VM_BACKREF;
    op->start = capture->start;
    op->end = capture->end;
    op->len = -1; // TODO: maybe calculate this? (nontrivial because of replacements)
    op->args.backref = capture;
    backref->op = op;
    backref->next = g->firstbackref;
    g->firstbackref = backref;
}

/*
 * Push all the backreferences contained in a match onto the backreference stack
 */
size_t push_backrefs(grammar_t *g, match_t *m)
{
    if (m->op->op == VM_REF) return 0;
    size_t count = 0;
    if (m->op->op == VM_CAPTURE && m->op->args.capture.name) {
        ++count;
        push_backref(g, m->op->args.capture.name, m->child);
    }
    if (m->child) count += push_backrefs(g, m->child);
    if (m->nextsibling) count += push_backrefs(g, m->nextsibling);
    return count;
}

/*
 * Pop a number of backreferences off the backreference stack
 */
void pop_backrefs(grammar_t *g, size_t count)
{
    for ( ; count > 0; count--) {
        backref_t *b = g->firstbackref;
        g->firstbackref = b->next;
        check(b, "Attempt to pop %ld more backrefs than there are", count);
        xfree((void**)&b->op);
        xfree((void**)&b);
    }
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
