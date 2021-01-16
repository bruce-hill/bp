//
// grammar.h - Header file defining grammars (sets of rule definitions)
//
#ifndef GRAMMAR__H
#define GRAMMAR__H

#include "files.h"
#include "types.h"

__attribute__((nonnull(2,4,5), returns_nonnull))
def_t *with_def(def_t *defs, file_t *f, size_t namelen, const char *name, pat_t *pat);
__attribute__((nonnull(2,3,4), returns_nonnull))
def_t *with_backref(def_t *defs, file_t *f, const char *name, match_t *m);
__attribute__((nonnull(2)))
def_t *load_grammar(def_t *defs, file_t *f);
__attribute__((pure, nonnull(2)))
def_t *lookup(def_t *defs, const char *name);
__attribute__((nonnull(1)))
void free_defs(def_t **defs, def_t *stop);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
