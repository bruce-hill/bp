/*
 * bpeg.h - Header file for the bpeg parser
 */
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utils.h"

const char *usage = "Usage: bpeg [-m|--multiline] [-v|--verbose] [-h|--help] [-s|--slow] <grammar> [<input file>]";
/*
 * Pattern matching result object
 */
typedef struct match_s {
    // Where the match starts and ends (end is after the last character)
    const char *start, *end;
    unsigned int is_capture:1, is_replacement:1;
    const char *name_or_replacement;
    struct match_s *child, *nextsibling;
} match_t;

/*
 * BPEG virtual machine opcodes
 */
enum VMOpcode {
    VM_EMPTY = 0,
    VM_ANYCHAR = 1,
    VM_STRING,
    VM_RANGE,
    VM_NOT,
    VM_UPTO,
    VM_UPTO_AND,
    VM_REPEAT,
    VM_BEFORE,
    VM_AFTER,
    VM_CAPTURE,
    VM_OTHERWISE,
    VM_CHAIN,
    VM_REPLACE,
    VM_REF,
};

/*
 * A struct reperesenting a BPEG virtual machine operation
 */
typedef struct vm_op_s {
    enum VMOpcode op;
    const char *start, *end;
    // Length of the match, if constant, otherwise -1
    ssize_t len;
    union {
        const char *s;
        struct {
            char low, high;
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
            struct vm_op_s *replace_pat;
            const char *replacement;
        } replace;
        struct {
            struct vm_op_s *capture_pat;
            char *name;
        } capture;
        struct vm_op_s *pat;
    } args;
} vm_op_t;


static inline const char *after_spaces(const char *str);
static match_t *free_match(match_t *m);
static match_t *match(const char *str, vm_op_t *op);
static vm_op_t *compile_bpeg(const char *source, const char *str);
static vm_op_t *load_grammar(const char *grammar);
static vm_op_t *chain_together(vm_op_t *first, vm_op_t *second);
static vm_op_t *compile_bpeg_string(const char *source, const char *str);
static vm_op_t *expand_chain(const char *source, vm_op_t *first);
static vm_op_t *expand_choices(const char *source, vm_op_t *op);
static void print_match(match_t *m, const char *color);
static void set_range(vm_op_t *op, ssize_t min, ssize_t max, vm_op_t *pat, vm_op_t *sep);


typedef struct {
    const char *name;
    const char *source;
    vm_op_t *op;
} def_t;

static def_t defs[1024] = {{NULL, NULL, NULL}};
size_t ndefs = 0;
