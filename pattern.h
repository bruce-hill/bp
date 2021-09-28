//
// pattern.h - Header file for BP pattern compilation.
//
#ifndef PATTERN__H
#define PATTERN__H

#include <stdbool.h>

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
    BP_DEFINITIONS   = 25,
    BP_LEFTRECURSION = 26,
};

//
// A struct reperesenting a BP virtual machine operation
//
typedef struct pat_s {
    struct pat_s *next, **home;
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
            struct pat_s *meaning, *next_def;
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
        struct {
            struct match_s *match;
            unsigned int visits;
            const char *at;
            struct pat_s *fallback;
        } leftrec;
        struct {
            const char *start, *end, *msg;
        } error;
        struct pat_s *pat;
    } args;
    size_t id;
} pat_t;

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
__attribute__((nonnull))
maybe_pat_t bp_stringpattern(const char *str, const char *end);
__attribute__((nonnull))
maybe_pat_t bp_replacement(pat_t *replacepat, const char *replacement, const char *end);
pat_t *chain_together(pat_t *first, pat_t *second);
pat_t *either_pat(pat_t *first, pat_t *second);
__attribute__((nonnull))
maybe_pat_t bp_pattern(const char *str, const char *end);
void free_all_pats(void);
__attribute__((nonnull))
void delete_pat(pat_t **at_pat, bool recursive);


#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
