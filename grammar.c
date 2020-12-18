/*
 * grammar.c - Code for defining grammars (sets of rules)
 */

#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "file_loader.h"
#include "grammar.h"
#include "utils.h"

grammar_t *new_grammar(void)
{
    grammar_t *g = new(grammar_t);
    g->definitions = xcalloc(sizeof(def_t), (g->defcapacity = 128));
    return g;
}

void add_def(grammar_t *g, file_t *f, const char *src, const char *name, vm_op_t *op)
{
    if (g->defcount >= g->defcapacity) {
        g->definitions = xrealloc(g->definitions, sizeof(&g->definitions[0])*(g->defcapacity += 32));
    }
    int i = g->defcount;
    g->definitions[i].file = f;
    g->definitions[i].source = src;
    g->definitions[i].name = name;
    g->definitions[i].op = op;
    ++g->defcount;
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
        vm_op_t *op = bpeg_pattern(f, src);
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
    if (src < &f->contents[f->length-1]) {
        fprint_line(stderr, f, src, NULL, "Invalid BPEG pattern");
        _exit(1);
    }
    return ret;
}

vm_op_t *lookup(grammar_t *g, const char *name)
{
    // Search backwards so newer backrefs/defs take precedence
    for (int i = (int)g->backrefcount-1; i >= 0; i--) {
        if (streq(g->backrefs[i].name, name)) {
            return g->backrefs[i].op;
        }
    }
    for (int i = g->defcount-1; i >= 0; i--) {
        if (streq(g->definitions[i].name, name)) {
            return g->definitions[i].op;
        }
    }
    return NULL;
}

void push_backref(grammar_t *g, const char *name, match_t *capture)
{
    if (g->backrefcount >= g->backrefcapacity) {
        g->backrefs = xrealloc(g->backrefs, sizeof(g->backrefs[0])*(g->backrefcapacity += 32));
    }
    size_t i = g->backrefcount++;
    g->backrefs[i].name = name;
    g->backrefs[i].capture = capture;
    vm_op_t *op = new(vm_op_t);
    op->op = VM_BACKREF;
    op->start = capture->start;
    op->end = capture->end;
    op->len = -1; // TODO: maybe calculate this?
    op->args.backref = (void*)capture;
    g->backrefs[i].op = op;
}

void pop_backrefs(grammar_t *g, size_t count)
{
    check(count <= g->backrefcount, "Attempt to pop %ld backrefs when there are only %ld", count, g->backrefcount);
    for ( ; count > 0; count--) {
        //free(g->backrefs[i].op); // TODO: memory leak problem??
        int i = (int)g->backrefcount - 1;
        memset(&g->backrefs[i], 0, sizeof(g->backrefs[i]));
        --g->backrefcount;
    }
}
