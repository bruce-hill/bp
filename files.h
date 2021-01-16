//
// files.h - Definitions of an API for loading files.
//
#ifndef FILES__H
#define FILES__H

#include <stdio.h>

struct allocated_pat_s; // declared in types.h

typedef struct file_s {
    struct file_s *next;
    const char *filename;
    char *contents, **lines, *end;
    size_t nlines;
    struct allocated_pat_s *pats;
    unsigned int mmapped:1;
} file_t;

__attribute__((format(printf,2,3)))
file_t *load_file(file_t **files, const char *fmt, ...);
__attribute__((nonnull(3), returns_nonnull))
file_t *spoof_file(file_t **files, const char *filename, const char *text);
__attribute__((nonnull))
void intern_file(file_t *f);
__attribute__((nonnull))
void destroy_file(file_t **f);
__attribute__((pure, nonnull))
size_t get_line_number(file_t *f, const char *p);
__attribute__((pure, nonnull))
size_t get_char_number(file_t *f, const char *p);
__attribute__((pure, nonnull))
const char *get_line(file_t *f, size_t line_number);
__attribute__((nonnull(1,2,3), format(printf,5,6)))
void fprint_line(FILE *dest, file_t *f, const char *start, const char *end, const char *fmt, ...);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1