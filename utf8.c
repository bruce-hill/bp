//
// utf8.c - UTF8 helper functions
//
#include "files.h"
#include "utf8.h"

//
// Return the location of the next character or UTF8 codepoint.
// (i.e. skip forward one codepoint at a time, not one byte at a time)
//
const char *next_char(file_t *f, const char *str)
{
    if (__builtin_expect(str+1 <= f->end && (str[0] & 0x80) == 0x0, 1))
        return str+1;
    if (__builtin_expect(str+2 <= f->end && (str[0] & 0xe0) == 0xc0, 1))
        return str+2;
    if (__builtin_expect(str+3 <= f->end && (str[0] & 0xf0) == 0xe0, 1))
        return str+3;
    if (__builtin_expect(str+4 <= f->end && (str[0] & 0xf8) == 0xf0, 1))
        return str+4;
    return __builtin_expect(str+1 <= f->end, 1) ? str+1 : f->end;
}

//
// Return the location of the previous character or UTF8 codepoint.
// (i.e. skip backwards one codepoint at a time, not one byte at a time)
//
const char *prev_char(file_t *f, const char *str)
{
    if (__builtin_expect(str-1 >= f->start && (str[-1] & 0x80) == 0x0, 1))
        return str-1;
    if (__builtin_expect(str-2 >= f->start && (str[-2] & 0xe0) == 0xc0, 1))
        return str-2;
    if (__builtin_expect(str-3 >= f->start && (str[-3] & 0xf0) == 0xe0, 1))
        return str-3;
    if (__builtin_expect(str-4 >= f->start && (str[-4] & 0xf8) == 0xf0, 1))
        return str-4;
    return __builtin_expect(str-1 >= f->start, 1) ? str-1 : f->start;
}
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
