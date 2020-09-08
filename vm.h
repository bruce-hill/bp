/*
 * vm.h - Source code for the BPEG virtual machine datatypes
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

typedef struct vm_op_s {
    enum VMOpcode op;
    const char *start, *end;
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

