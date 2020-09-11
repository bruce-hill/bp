/*
 * compiler.h - Header file for BPEG compiler.
 */
#ifndef COMPILER__H
#define COMPILER__H

#include <stdlib.h>

#include "types.h"

vm_op_t *bpeg_simplepattern(const char *str);
vm_op_t *bpeg_stringpattern(const char *str);
vm_op_t *bpeg_replacement(vm_op_t *pat, const char *replacement);
vm_op_t *bpeg_pattern(const char *str);

#endif
