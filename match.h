//
// match.h - Header file for BP virtual machine.
//
#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

#include "pattern.h"

//
// Pattern matching result object
//
typedef struct match_s {
    // Where the match starts and ends (end is after the last character)
    const char *start, *end;
    pat_t *pat;
    // Intrusive linked list node for garbage collection:
    struct {
        struct match_s **home, *next;
    } gc;
    struct match_s **children;
    struct match_s *_children[3];
} match_t;

typedef void (*bp_errhand_t)(char **err_msg);

__attribute__((nonnull))
void recycle_match(match_t **at_m);
size_t free_all_matches(void);
size_t recycle_all_matches(void);
bool next_match(match_t **m, const char *start, const char *end, pat_t *pat, pat_t *defs, pat_t *skip, bool ignorecase);
#define stop_matching(m) next_match(m, NULL, NULL, NULL, NULL, NULL, 0)
bp_errhand_t bp_set_error_handler(bp_errhand_t handler);
__attribute__((nonnull))
match_t *get_numbered_capture(match_t *m, int n);
__attribute__((nonnull, pure))
match_t *get_named_capture(match_t *m, const char *name, ssize_t namelen);

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
