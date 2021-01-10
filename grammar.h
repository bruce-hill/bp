/*
 * grammar.h - Header file defining grammars (sets of rule definitions)
 */
#ifndef GRAMMAR__H
#define GRAMMAR__H

#include "file_loader.h"
#include "types.h"

__attribute__((nonnull(1,3,4,5)))
void add_def(grammar_t *g, file_t *f, const char *src, const char *name, vm_op_t *op);
__attribute__((nonnull))
void push_backref(grammar_t *g, const char *name, match_t *capture);
__attribute__((nonnull))
size_t push_backrefs(grammar_t *g, match_t *m);
__attribute__((nonnull))
void pop_backrefs(grammar_t *g, size_t count);
__attribute__((nonnull))
vm_op_t *load_grammar(grammar_t *g, file_t *f);
__attribute__((pure, nonnull))
vm_op_t *lookup(grammar_t *g, const char *name);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
