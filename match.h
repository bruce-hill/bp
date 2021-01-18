//
// match.h - Header file for BP virtual machine.
//
#ifndef MATCH__H
#define MATCH__H

#include <stdbool.h>
#include <stdio.h>

#include "types.h"

__attribute__((nonnull(2,4)))
match_t *next_match(def_t *defs, file_t *f, match_t *prev, pat_t *pat, bool ignorecase);
__attribute__((nonnull))
match_t *get_capture(match_t *m, const char **id);
__attribute__((nonnull))
void recycle_if_unused(match_t **at_m);
#ifdef DEBUG_HEAP
size_t free_all_matches(void);
size_t recycle_all_matches(void);
#endif

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
