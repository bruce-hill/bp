//
// pattern.h - Header file for BP pattern compilation.
//
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

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
typedef struct pat_s {
    struct pat_s *next, **home;
    enum pattype_e type;
    uint32_t id;
    const char *start, *end;
    // The bounds of the match length (used for backtracking)
    uint32_t min_matchlen;
    int32_t max_matchlen; // -1 means unbounded length
    union {
        const char *string;
        struct {
            const char *name;
            uint32_t len;
        } ref;
        struct {
            const char *name;
            uint32_t namelen;
            struct pat_s *meaning, *next_def;
        } def;
        struct {
            unsigned char low, high;
        } range;
        struct {
            uint32_t min;
            int32_t max;
            struct pat_s *sep, *repeat_pat;
        } repetitions;
        // TODO: use a linked list instead of a binary tree
        struct {
            struct pat_s *first, *second;
        } multiple;
        struct {
            struct pat_s *pat;
            const char *text;
            uint32_t len;
        } replace;
        struct {
            struct pat_s *capture_pat;
            const char *name;
            uint16_t namelen;
            bool backreffable;
        } capture;
        struct leftrec_info_s *leftrec;
        struct {
            const char *start, *end, *msg;
        } error;
        struct pat_s *pat;
    } args;
} pat_t;

typedef struct leftrec_info_s {
    struct match_s *match;
    const char *at;
    struct pat_s *fallback;
    void *ctx;
    bool visited;
} leftrec_info_t;

typedef struct {
    bool success;
    union {
        pat_t *pat;
        struct {
            const char *start, *end, *msg;
        } error;
    } value;
} maybe_pat_t;

__attribute__((nonnull, returns_nonnull))
pat_t *bp_raw_literal(const char *str, size_t len);
__attribute__((nonnull(1)))
maybe_pat_t bp_stringpattern(const char *str, const char *end);
__attribute__((nonnull(1,2)))
maybe_pat_t bp_replacement(pat_t *replacepat, const char *replacement, const char *end);
pat_t *chain_together(pat_t *first, pat_t *second);
pat_t *either_pat(pat_t *first, pat_t *second);
__attribute__((nonnull(1)))
maybe_pat_t bp_pattern(const char *str, const char *end);
void free_all_pats(void);
__attribute__((nonnull))
void delete_pat(pat_t **at_pat, bool recursive);

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
