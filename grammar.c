/*
 * grammar.c - Code for defining grammars (sets of rules)
 */

#include "grammar.h"
#include "compiler.h"
#include "utils.h"

grammar_t *new_grammar(void)
{
    grammar_t *g = calloc(sizeof(grammar_t), 1);
    g->definitions = calloc(sizeof(def_t), (g->capacity = 128));
    return g;
}

void add_def(grammar_t *g, const char *src, const char *name, vm_op_t *op)
{
    if (g->size >= g->capacity) {
        g->definitions = realloc(g->definitions, (g->capacity += 32));
    }
    int i = g->size;
    g->definitions[i].source = src;
    g->definitions[i].name = name;
    g->definitions[i].op = op;
    ++g->size;
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
    // Search backwards so newer defs take precedence
    for (int i = g->size-1; i >= 0; i--) {
        if (streq(g->definitions[i].name, name)) {
            return g->definitions[i].op;
        }
    }
    return NULL;
}
