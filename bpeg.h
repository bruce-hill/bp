/*
 * bpeg.h - Header file for the bpeg parser
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utils.h"
#include "vm.h"

typedef struct match_s {
    const char *start, *end;
    union {
        unsigned int is_capture:1;
        const char *name;
    } capture;
    const char *replacement;
    struct match_s *child, *nextsibling;
} match_t;

static match_t *free_match(match_t *m);
static match_t *match(const char *str, vm_op_t *op);
static void set_range(vm_op_t *op, ssize_t min, ssize_t max, vm_op_t *pat, vm_op_t *sep);
static inline const char *after_spaces(const char *str);
static vm_op_t *expand_choices(vm_op_t *op);
static vm_op_t *expand_chain(vm_op_t *first);
static vm_op_t *compile_bpeg(const char *str);


typedef struct {
    const char *name;
    vm_op_t *op;
} def_t;

static def_t defs[1024] = {{NULL, NULL}};
size_t ndefs = 0;
static int verbose = 1;

