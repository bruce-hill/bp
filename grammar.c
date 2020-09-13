/*
 * grammar.c - Code for defining grammars (sets of rules)
 */

#include "grammar.h"
#include "compiler.h"
#include "utils.h"

grammar_t *new_grammar(void)
{
    grammar_t *g = calloc(sizeof(grammar_t), 1);
    g->definitions = calloc(sizeof(def_t), (g->defcapacity = 128));
    return g;
}

void add_def(grammar_t *g, const char *src, const char *name, vm_op_t *op)
{
    if (g->defcount >= g->defcapacity) {
        g->definitions = realloc(g->definitions, sizeof(&g->definitions[0])*(g->defcapacity += 32));
    }
    int i = g->defcount;
    g->definitions[i].source = src;
    g->definitions[i].name = name;
    g->definitions[i].op = op;
    ++g->defcount;
}

/*
 * Load the given grammar (semicolon-separated definitions)
 * and return the first rule defined.
 */
vm_op_t *load_grammar(grammar_t *g, const char *src)
{
    vm_op_t *ret = NULL;
    do {
        src = after_spaces(src);
        if (!*src) break;
        const char *name = src;
        const char *name_end = after_name(name);
        check(name_end > name, "Invalid name for definition");
        name = strndup(name, (size_t)(name_end-name));
        src = after_spaces(name_end);
        check(matchchar(&src, '='), "Expected '=' in definition");
        vm_op_t *op = bpeg_pattern(src);
        check(op, "Couldn't load definition");
        add_def(g, src, name, op);
        if (ret == NULL) {
            ret = op;
        }
        src = op->end;
    } while (*src && matchchar(&src, ';')); 
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
    check(capture, "No capture provided");
    if (g->backrefcount >= g->backrefcapacity) {
        g->backrefs = realloc(g->backrefs, sizeof(g->backrefs[0])*(g->backrefcapacity += 32));
    }
    size_t i = g->backrefcount++;
    g->backrefs[i].name = name;
    g->backrefs[i].capture = capture;
    vm_op_t *op = calloc(sizeof(vm_op_t), 1);
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
