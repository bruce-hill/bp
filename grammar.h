/*
 * grammar.h - Header file defining grammars (sets of rule definitions)
 */
#ifndef GRAMMAR__H
#define GRAMMAR__H

#include <stdlib.h>
#include <string.h>

#include "types.h"

grammar_t *new_grammar(void);
void add_def(grammar_t *g, const char *src, const char *name, vm_op_t *op);
void push_backref(grammar_t *g, const char *name, match_t *capture);
void pop_backrefs(grammar_t *g, size_t count);
vm_op_t *load_grammar(grammar_t *g, const char *source);
vm_op_t *lookup(grammar_t *g, const char *name);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
