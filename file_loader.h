/*
 * file_loader.h - Definitions of an API for loading files.
 */
#ifndef FILE_LOADER__H
#define FILE_LOADER__H

#include <stdio.h>

typedef struct {
    const char *filename;
    char *contents, **lines, *end;
    size_t length, nlines;
    unsigned int mmapped:1;
} file_t;

file_t *load_file(const char *filename);
file_t *spoof_file(const char *filename, char *text);
__attribute__((nonnull))
void destroy_file(file_t **f);
__attribute__((pure, nonnull))
size_t get_line_number(file_t *f, const char *p);
__attribute__((pure, nonnull))
size_t get_char_number(file_t *f, const char *p);
__attribute__((pure, nonnull))
const char *get_line(file_t *f, size_t line_number);
__attribute__((format (printf, 5, 6)))
void fprint_line(FILE *dest, file_t *f, const char *start, const char *end, const char *fmt, ...);

#endif
