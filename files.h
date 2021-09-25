//
// files.h - Definitions of an API for loading files.
//
#ifndef FILES__H
#define FILES__H

#include <stdio.h>
#include <unistd.h>

typedef struct file_s {
    struct file_s *next;
    const char *filename;
    char *mmapped, *allocated;
    char **lines, *start, *end;
    size_t nlines;
} file_t;

__attribute__((nonnull(2)))
file_t *load_file(file_t **files, const char *filename);
__attribute__((format(printf,2,3)))
file_t *load_filef(file_t **files, const char *fmt, ...);
__attribute__((nonnull))
void slice_file(file_t *slice, file_t *src, const char *start, const char *end);
__attribute__((nonnull(3), returns_nonnull))
file_t *spoof_file(file_t **files, const char *filename, const char *text, ssize_t len);
__attribute__((nonnull))
void destroy_file(file_t **f);
__attribute__((pure, nonnull))
size_t get_line_number(file_t *f, const char *p);
__attribute__((pure, nonnull))
const char *get_line(file_t *f, size_t line_number);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
