//
// match.h - Header file for BP virtual machine.
//
#ifndef MATCH__H
#define MATCH__H

#include <stdbool.h>
#include <stdio.h>

#include "files.h"
#include "types.h"

__attribute__((returns_nonnull))
match_t *new_match(def_t *defs, pat_t *pat, const char *start, const char *end, match_t *children[]);
__attribute__((nonnull(2,4)))
match_t *next_match(def_t *defs, file_t *f, match_t *prev, pat_t *pat, pat_t *skip, bool ignorecase);
__attribute__((nonnull))
match_t *get_capture(match_t *m, const char **id);
__attribute__((nonnull))
void recycle_if_unused(match_t **at_m);
size_t free_all_matches(void);
size_t recycle_all_matches(void);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
