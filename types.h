//
// types.h - Datatypes used by BP
//
#ifndef TYPES__H
#define TYPES__H

#include <sys/types.h>

#include "file_loader.h"

//
// BP virtual machine opcodes (these must be kept in sync with the names in vm.c)
//
enum VMOpcode {
    VM_ANYCHAR = 1,
    VM_STRING,
    VM_RANGE,
    VM_NOT,
    VM_UPTO_AND,
    VM_REPEAT,
    VM_BEFORE,
    VM_AFTER,
    VM_CAPTURE,
    VM_OTHERWISE,
    VM_CHAIN,
    VM_EQUAL,
    VM_NOT_EQUAL,
    VM_REPLACE,
    VM_REF,
    VM_BACKREF,
    VM_NODENT,
    VM_LEFTRECURSION,
};

struct match_s; // forward declared to resolve circular struct defs

//
// A struct reperesenting a BP virtual machine operation
//
typedef struct pat_s {
    enum VMOpcode type;
    const char *start, *end;
    // Length of the match, if constant, otherwise -1
    ssize_t len;
    union {
        const char *s;
        struct {
            unsigned char low, high;
        } range;
        struct {
            ssize_t min, max;
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
            char *name;
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
    pat_t *op;
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
    unsigned int skip_replacement:1;
} match_t;

//
// Pattern matching rule definition(s)
//
typedef struct def_s {
    size_t namelen;
    const char *name;
    file_t *file;
    pat_t *op;
    struct def_s *next;
} def_t;

//
// Structure used for tracking allocated ops, which must be freed when the file
// is freed.
//
typedef struct allocated_op_s {
    struct allocated_op_s *next;
    pat_t op;
} allocated_op_t;

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
