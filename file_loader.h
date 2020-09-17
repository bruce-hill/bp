/*
 * file_loader.h - Definitions of an API for loading files.
 */
#ifndef FILE_LOADER__H
#define FILE_LOADER__H

#include <stdio.h>

typedef struct {
    const char *filename;
    char *contents, **lines;
    size_t length, nlines;
    unsigned int mmapped:1;
} file_t;

file_t *load_file(const char *filename);
void destroy_file(file_t **f);
size_t get_line_number(file_t *f, const char *p);
const char *get_line(file_t *f, size_t line_number);
void fprint_line(FILE *dest, file_t *f, const char *start, const char *end, const char *msg);

#endif
