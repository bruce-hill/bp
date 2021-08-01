//
// types.h - Datatypes used by BP
//
#ifndef TYPES__H
#define TYPES__H

#include <stdbool.h>
#include <sys/types.h>

#include "files.h"

#define UNBOUNDED(pat) ((pat)->max_matchlen == -1)

// BP virtual machine pattern types
enum pattype_e {
    BP_ANYCHAR       = 1,
    BP_ID_START      = 2,
    BP_ID_CONTINUE   = 3,
    BP_STRING        = 4,
    BP_RANGE         = 5,
    BP_NOT           = 6,
    BP_UPTO          = 7,
    BP_UPTO_STRICT   = 8,
    BP_REPEAT        = 9,
    BP_BEFORE        = 10,
    BP_AFTER         = 11,
    BP_CAPTURE       = 12,
    BP_OTHERWISE     = 13,
    BP_CHAIN         = 14,
    BP_MATCH         = 15,
    BP_NOT_MATCH     = 16,
    BP_REPLACE       = 17,
    BP_REF           = 18,
    BP_NODENT        = 19,
    BP_START_OF_FILE = 20,
    BP_START_OF_LINE = 21,
    BP_END_OF_FILE   = 22,
    BP_END_OF_LINE   = 23,
    BP_WORD_BOUNDARY = 24,
    BP_DEFINITION    = 25,
    BP_LEFTRECURSION = 26,
    BP_ERROR         = 27,
};

struct match_s; // forward declared to resolve circular struct defs

//
// A struct reperesenting a BP virtual machine operation
//
typedef struct pat_s {
    struct pat_s *next;
    enum pattype_e type;
    const char *start, *end;
    // The bounds of the match length (used for backtracking)
    size_t min_matchlen;
    ssize_t max_matchlen; // -1 means unbounded length
    union {
        const char *string;
        struct {
            const char *name;
            size_t len;
        } ref;
        struct {
            const char *name;
            size_t namelen;
            struct pat_s *def, *pat;
        } def;
        struct {
            unsigned char low, high;
        } range;
        struct {
            size_t min;
            ssize_t max;
            struct pat_s *sep, *repeat_pat;
        } repetitions;
        // TODO: use a linked list instead of a binary tree
        struct {
            struct pat_s *first, *second;
        } multiple;
        struct {
            struct pat_s *pat;
            const char *text;
            size_t len;
        } replace;
        struct {
            struct pat_s *capture_pat;
            const char *name;
            size_t namelen;
        } capture;
        struct match_s *backref;
        struct {
            struct match_s *match;
            unsigned int visits;
            const char *at;
            struct pat_s *fallback;
        } leftrec;
        struct pat_s *pat;
    } args;
    size_t id;
} pat_t;

typedef struct {
    struct match_s **home, *next;
} match_dll_t;

// #define MATCH_FROM(node, name) ((match_t*)((char*)node + (size_t)(&((match_t*)0)->name)))

//
// Pattern matching result object
//
typedef struct match_s {
    // Where the match starts and ends (end is after the last character)
    const char *start, *end;
    pat_t *pat;
    // Intrusive linked list nodes for garbage collection and cache buckets:
    match_dll_t gc, cache;
    size_t defs_id;
    int refcount;
    // If skip_replacement is set to 1, that means the user wants to not print
    // the replaced text when printing this match:
    // TODO: this is a bit hacky, there is probably a better way to go about
    // this but it's less hacky that mutating the match objects more drastically
    bool skip_replacement:1;
    struct match_s **children;
    struct match_s *_children[3];
} match_t;

//
// Pattern matching rule definition(s)
//
typedef struct def_s {
    size_t id;
    size_t namelen;
    const char *name;
    pat_t *pat;
    struct def_s *next;
} def_t;

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
