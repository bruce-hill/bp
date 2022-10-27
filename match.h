//
// match.h - Header file for BP virtual machine.
//
#ifndef MATCH__H
#define MATCH__H

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

typedef void (*bp_errhand_t)(const char *err_msg);

typedef enum {BP_STOP = 0, BP_CONTINUE} bp_match_behavior;
typedef bp_match_behavior (*bp_match_callback)(match_t *m, int matchnum, void *userdata);
int each_match(bp_match_callback fn, void *userdata, const char *start, const char *end, pat_t *pat, pat_t *defs, pat_t *skip, bool ignorecase);
bp_errhand_t set_match_error_handler(bp_errhand_t errhand);

__attribute__((nonnull))
match_t *get_numbered_capture(match_t *m, int n);
__attribute__((nonnull, pure))
match_t *get_named_capture(match_t *m, const char *name, ssize_t namelen);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
