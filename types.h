/*
 * types.h - Datatypes used by BP
 */
#ifndef TYPES__H
#define TYPES__H

#include <sys/types.h>

#include "file_loader.h"

enum BPFlag {
    BP_VERBOSE    = 1 << 0,
    BP_IGNORECASE = 1 << 1,
    BP_EXPLAIN    = 1 << 2,
    BP_JSON       = 1 << 3,
    BP_LISTFILES  = 1 << 4,
    BP_INPLACE    = 1 << 5,
};

/*
 * BP virtual machine opcodes (these must be kept in sync with the names in vm.c)
 */
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
    VM_HIDE,
    VM_OTHERWISE,
    VM_CHAIN,
    VM_EQUAL,
    VM_NOT_EQUAL,
    VM_REPLACE,
    VM_REF,
    VM_BACKREF,
    VM_NODENT,
};

struct match_s; // forward declared to resolve circular struct defs

/*
 * A struct reperesenting a BP virtual machine operation
 */
typedef struct vm_op_s {
    enum VMOpcode op;
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
            struct vm_op_s *sep, *repeat_pat;
        } repetitions;
        // TODO: use a linked list instead of a binary tree
        struct {
            struct vm_op_s *first, *second;
        } multiple;
        struct {
            struct vm_op_s *pat;
            const char *text;
            size_t len;
        } replace;
        struct {
            struct vm_op_s *capture_pat;
            char *name;
        } capture;
        struct match_s *backref;
        struct vm_op_s *pat;
    } args;
} vm_op_t;

/*
 * Pattern matching result object
 */
typedef struct match_s {
    // Where the match starts and ends (end is after the last character)
    const char *start, *end;
    struct match_s *child, *nextsibling;
    vm_op_t *op;
} match_t;

/*
 * Pattern matching rule definition
 */
typedef struct def_s {
    const char *name;
    const char *source;
    file_t *file;
    vm_op_t *op;
    struct def_s *next;
} def_t;

/*
 * Backreference (look up previous capture by name)
 */
typedef struct backref_s {
    const char *name;
    match_t *capture;
    vm_op_t *op;
    struct backref_s *next;
} backref_t;

/*
 * Grammar (a collection of definitions)
 */
typedef struct {
    def_t *firstdef;
    backref_t *firstbackref;
} grammar_t;

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
