//
// pattern.h - Header file for BP pattern compilation.
//
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <err.h>

// BP virtual machine pattern types
enum bp_pattype_e {
    BP_ERROR         = 0,
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
    BP_CURDENT       = 20,
    BP_START_OF_FILE = 21,
    BP_START_OF_LINE = 22,
    BP_END_OF_FILE   = 23,
    BP_END_OF_LINE   = 24,
    BP_WORD_BOUNDARY = 25,
    BP_DEFINITIONS   = 26,
    BP_TAGGED        = 27,
    BP_LEFTRECURSION = 28,
};

//
// A struct reperesenting a BP virtual machine operation
//
typedef struct bp_pat_s bp_pat_t;
struct bp_pat_s {
    bp_pat_t *next, **home;
    enum bp_pattype_e type;
    uint32_t id;
    const char *start, *end;
    // The bounds of the match length (used for backtracking)
    uint32_t min_matchlen;
    int32_t max_matchlen; // -1 means unbounded length
    union {
        struct {
            const char *start, *end, *msg;
        } BP_ERROR;
        struct {} BP_ANYCHAR;
        struct {} BP_ID_START;
        struct {} BP_ID_CONTINUE;
        struct {const char *string; size_t len; } BP_STRING;
        struct {unsigned char low, high; } BP_RANGE;
        struct {bp_pat_t *pat;} BP_NOT;
        struct {bp_pat_t *target, *skip;} BP_UPTO;
        struct {bp_pat_t *target, *skip;} BP_UPTO_STRICT;
        struct {
            uint32_t min;
            int32_t max;
            bp_pat_t *sep, *repeat_pat;
        } BP_REPEAT;
        struct {bp_pat_t *pat;} BP_BEFORE;
        struct {bp_pat_t *pat;} BP_AFTER;
        struct {
            bp_pat_t *pat;
            const char *name;
            uint16_t namelen;
            bool backreffable;
        } BP_CAPTURE;
        struct {
            bp_pat_t *first, *second;
        } BP_OTHERWISE;
        struct {
            bp_pat_t *first, *second;
        } BP_CHAIN;
        struct {bp_pat_t *pat, *must_match;} BP_MATCH;
        struct {bp_pat_t *pat, *must_not_match;} BP_NOT_MATCH;
        struct {
            bp_pat_t *pat;
            const char *text;
            uint32_t len;
        } BP_REPLACE;
        struct {
            const char *name;
            uint32_t len;
        } BP_REF;
        struct {} BP_NODENT;
        struct {} BP_CURDENT;
        struct {} BP_START_OF_FILE;
        struct {} BP_START_OF_LINE;
        struct {} BP_END_OF_FILE;
        struct {} BP_END_OF_LINE;
        struct {} BP_WORD_BOUNDARY;
        struct {
            const char *name;
            uint32_t namelen;
            bp_pat_t *meaning, *next_def;
        } BP_DEFINITIONS;
        struct {
            bp_pat_t *pat;
            const char *name;
            uint16_t namelen;
            bool backreffable;
        } BP_TAGGED;
        struct {
            struct bp_match_s *match;
            const char *at;
            bp_pat_t *fallback;
            void *ctx;
            bool visited;
        } BP_LEFTRECURSION;
    } __tagged;
};

typedef struct leftrec_info_s {
    struct bp_match_s *match;
    const char *at;
    bp_pat_t *fallback;
    void *ctx;
    bool visited;
} leftrec_info_t;

typedef struct {
    bool success;
    union {
        bp_pat_t *pat;
        struct {
            const char *start, *end, *msg;
        } error;
    } value;
} maybe_pat_t;

__attribute__((returns_nonnull))
bp_pat_t *allocate_pat(bp_pat_t pat);
__attribute__((nonnull, returns_nonnull))
bp_pat_t *bp_raw_literal(const char *str, size_t len);
__attribute__((nonnull(1)))
maybe_pat_t bp_stringpattern(const char *str, const char *end);
__attribute__((nonnull(1,2)))
maybe_pat_t bp_replacement(bp_pat_t *replacepat, const char *replacement, const char *end);
bp_pat_t *chain_together(bp_pat_t *first, bp_pat_t *second);
bp_pat_t *either_pat(bp_pat_t *first, bp_pat_t *second);
__attribute__((nonnull(1)))
maybe_pat_t bp_pattern(const char *str, const char *end);
void free_all_pats(void);
__attribute__((nonnull))
void delete_pat(bp_pat_t **at_pat, bool recursive);
int fprint_pattern(FILE *stream, bp_pat_t *pat);

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
