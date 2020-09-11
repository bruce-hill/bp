/*
 * grammar.h - Header file defining grammars (sets of rule definitions)
 */
#ifndef GRAMMAR__H
#define GRAMMAR__H

#include <stdlib.h>
#include <string.h>

#include "types.h"

grammar_t *new_grammar(void);
void add_def(grammar_t *g, const char *src, const char *name, vm_op_t *op);
void load_grammar(grammar_t *g, const char *source);
void print_grammar(grammar_t *g);


#endif
