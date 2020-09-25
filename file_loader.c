/*
 * file_loader.c - Implementation of some file loading functionality.
 */

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "file_loader.h"

/*
 * Read an entire file into memory.
 */
file_t *load_file(const char *filename)
{
    if (filename == NULL) filename = "-";
    int fd = strcmp(filename, "-") != 0 ? open(filename, O_RDONLY) : STDIN_FILENO;
    if (fd < 0) return NULL;
    file_t *f = calloc(sizeof(file_t), 1);
    f->filename = strdup(filename);
    // TODO: use mmap when possible
    struct stat sb;
    if (fstat(fd, &sb) == -1)
        goto skip_mmap;

    f->contents = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (f->contents == MAP_FAILED)
        goto skip_mmap;

    f->mmapped = 1;
    f->length = (size_t)sb.st_size;
    goto finished_loading;

  skip_mmap:
    f->mmapped = 0;
    size_t capacity = 1000;
    f->length = 0;
    f->contents = calloc(sizeof(char), capacity);
    ssize_t just_read;
    while ((just_read=read(fd, &f->contents[f->length], capacity - f->length)) > 0) {
        f->length += (size_t)just_read;
        if (f->length >= capacity)
            f->contents = realloc(f->contents, sizeof(char)*(capacity *= 2) + 1);
    }
    close(fd);

  finished_loading:
    f->end = &f->contents[f->length];
    
    // Calculate line numbers:
    size_t linecap = 10;
    f->lines = calloc(sizeof(const char*), linecap);
    f->nlines = 0;
    char *p = f->contents;
    for (size_t n = 0; p && *p; ++n) {
        ++f->nlines;
        if (n >= linecap)
            f->lines = realloc(f->lines, sizeof(const char*)*(linecap *= 2));
        f->lines[n] = p;
        p = strchr(p, '\n');
        if (p) ++p;
    }

    return f;
}

void destroy_file(file_t **f)
{
    if ((*f)->filename) {
        free((char*)(*f)->filename);
        (*f)->filename = NULL;
    }
    if ((*f)->lines) {
        free((*f)->lines);
        (*f)->lines = NULL;
    }
    if ((*f)->contents) {
        if ((*f)->mmapped) {
            munmap((*f)->contents, (*f)->length);
        } else {
            free((*f)->contents);
        }
        (*f)->contents = NULL;
    }
    free(*f);
    *f = NULL;
}

size_t get_line_number(file_t *f, const char *p)
{
    // TODO: binary search
    for (size_t n = 1; n < f->nlines; n++) {
        if (f->lines[n] > p)
            return n;
    }
    return f->nlines;
}

size_t get_char_number(file_t *f, const char *p)
{
    size_t linenum = get_line_number(f, p);
    return 1 + (size_t)(p - f->lines[linenum-1]);
}

const char *get_line(file_t *f, size_t line_number)
{
    if (line_number == 0 || line_number > f->nlines) return NULL;
    return f->lines[line_number - 1];
}

void fprint_line(FILE *dest, file_t *f, const char *start, const char *end, const char *msg)
{
    size_t linenum = get_line_number(f, start);
    const char *line = get_line(f, linenum);
    size_t charnum = get_char_number(f, start);
    fprintf(dest, "\033[1m%s:%ld:\033[0m %s\n", f->filename, linenum, msg);
    const char *eol = linenum == f->nlines ? strchr(line, '\0') : strchr(line, '\n');
    if (end == NULL || end > eol) end = eol;
    fprintf(dest, "\033[2m% 5ld |\033[0m %.*s\033[41;30m%.*s\033[0m%.*s\n",
            linenum,
            (int)charnum - 1, line,
            (int)(end - &line[charnum-1]), &line[charnum-1],
            (int)(eol - end), end);
    fprintf(dest, "       \033[34;1m");
    const char *p = line - 1;
    for (; p < start; ++p) fputc(' ', dest);
    if (start == end) ++end;
    for (; p < end; ++p) fputc('^', dest);
    fprintf(dest, "\033[0m\n");
}
