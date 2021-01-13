//
// grammar.h - Header file defining grammars (sets of rule definitions)
//
#ifndef GRAMMAR__H
#define GRAMMAR__H

#include "file_loader.h"
#include "types.h"

__attribute__((nonnull(2,3,4), returns_nonnull))
def_t *with_def(def_t *defs, file_t *f, const char *name, vm_op_t *op);
__attribute__((nonnull(2,3)))
def_t *with_backrefs(def_t *defs, file_t *f, match_t *m);
__attribute__((nonnull(2)))
def_t *load_grammar(def_t *defs, file_t *f);
__attribute__((pure, nonnull(2)))
vm_op_t *lookup(def_t *defs, const char *name);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
