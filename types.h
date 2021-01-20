//
// types.h - Datatypes used by BP
//
#ifndef TYPES__H
#define TYPES__H

#include <stdbool.h>
#include <sys/types.h>

#include "files.h"

// BP virtual machine pattern types
enum pattype_e {
    BP_ANYCHAR = 1,
    BP_STRING,
    BP_RANGE,
    BP_NOT,
    BP_UPTO,
    BP_REPEAT,
    BP_BEFORE,
    BP_AFTER,
    BP_CAPTURE,
    BP_OTHERWISE,
    BP_CHAIN,
    BP_EQUAL,
    BP_NOT_EQUAL,
    BP_REPLACE,
    BP_REF,
    BP_BACKREF,
    BP_NODENT,
    BP_LEFTRECURSION,
};

struct match_s; // forward declared to resolve circular struct defs

//
// A struct reperesenting a BP virtual machine operation
//
typedef struct pat_s {
    enum pattype_e type;
    const char *start, *end;
    // Length of the match, if constant, otherwise -1
    ssize_t len;
    union {
        const char *string;
        struct {
            const char *name;
            size_t len;
        } name;
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
} pat_t;

//
// Pattern matching result object
//
typedef struct match_s {
    // Where the match starts and ends (end is after the last character)
    const char *start, *end;
    struct match_s *child, *nextsibling;
    pat_t *pat;
    // Intrusive linked list nodes for garbage collection:
    struct match_s *next;
#ifdef DEBUG_HEAP
    struct match_s **atme;
#endif
    int refcount;
    // If skip_replacement is set to 1, that means the user wants to not print
    // the replaced text when printing this match:
    // TODO: this is a bit hacky, there is probably a better way to go about
    // this but it's less hacky that mutating the match objects more drastically
    bool skip_replacement:1;
} match_t;

//
// Pattern matching rule definition(s)
//
typedef struct def_s {
    size_t namelen;
    const char *name;
    pat_t *pat;
    struct def_s *next;
} def_t;

//
// Structure used for tracking allocated patterns, which must be freed when the
// file is freed.
//
typedef struct allocated_pat_s {
    struct allocated_pat_s *next;
    pat_t pat;
} allocated_pat_t;

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
