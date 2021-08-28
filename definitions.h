//
// definitions.h - Header file defining pattern rules
//
#ifndef DEFINITIONS__H
#define DEFINITIONS__H

#include "files.h"
#include "types.h"

__attribute__((nonnull(3,4), returns_nonnull))
def_t *with_def(def_t *defs, size_t namelen, const char *name, pat_t *pat);
__attribute__((nonnull(2)))
def_t *load_grammar(def_t *defs, file_t *f);
__attribute__((pure, nonnull(3)))
def_t *lookup(def_t *defs, size_t namelen, const char *name);
__attribute__((nonnull(1)))
def_t *free_defs(def_t *defs, def_t *stop);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
