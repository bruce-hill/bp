/*
 * compiler.h - Header file for BPEG compiler.
 */
#ifndef COMPILER__H
#define COMPILER__H

#include <stdlib.h>

#include "types.h"
#include "file_loader.h"

vm_op_t *bpeg_simplepattern(file_t *f, const char *str);
vm_op_t *bpeg_stringpattern(file_t *f, const char *str);
vm_op_t *bpeg_replacement(vm_op_t *pat, const char *replacement);
vm_op_t *bpeg_pattern(file_t *f, const char *str);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
