//
// files.c - Implementation of some file loading functionality.
//

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files.h"
#include "pattern.h"
#include "utils.h"

__attribute__((nonnull))
static void populate_lines(file_t *f);
__attribute__((pure, nonnull))
static size_t get_char_number(file_t *f, const char *p);

//
// In the file object, populate the `lines` array with pointers to the
// beginning of each line.
//
static void populate_lines(file_t *f)
{
    // Calculate line numbers:
    size_t linecap = 10;
    f->lines = xcalloc(sizeof(const char*), linecap);
    f->nlines = 0;
    char *p = f->contents;
    for (size_t n = 0; p && p < f->end; ++n) {
        ++f->nlines;
        if (n >= linecap)
            f->lines = xrealloc(f->lines, sizeof(const char*)*(linecap *= 2));
        f->lines[n] = p;
        p = strchr(p, '\n');
        if (p) ++p;
    }
}

//
// Read an entire file into memory, using a printf-style formatting string to
// construct the filename.
//
file_t *load_filef(file_t **files, const char *fmt, ...)
{
    char filename[PATH_MAX+1] = {0};
    va_list args;
    va_start(args, fmt);
    check(vsnprintf(filename, PATH_MAX, fmt, args) <= (int)PATH_MAX,
          "File name is too large");
    va_end(args);
    return load_file(files, filename);
}

//
// Read an entire file into memory.
//
file_t *load_file(file_t **files, const char *filename)
{
    int fd = filename[0] == '\0' ? STDIN_FILENO : open(filename, O_RDONLY);
    if (fd < 0) return NULL;
    size_t length;
    file_t *f = new(file_t);
    f->filename = strdup(filename);

    struct stat sb;
    if (fstat(fd, &sb) == -1)
        goto skip_mmap;

    f->contents = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (f->contents == MAP_FAILED)
        goto skip_mmap;

    f->mmapped = 1;
    length = (size_t)sb.st_size;
    goto finished_loading;

  skip_mmap:
    f->mmapped = 0;
    size_t capacity = 1000;
    length = 0;
    f->contents = xcalloc(sizeof(char), capacity);
    ssize_t just_read;
    while ((just_read=read(fd, &f->contents[length], capacity - length)) > 0) {
        length += (size_t)just_read;
        if (length >= capacity)
            f->contents = xrealloc(f->contents, sizeof(char)*(capacity *= 2) + 1);
    }

  finished_loading:
    if (fd != STDIN_FILENO) check(close(fd) == 0, "Failed to close file");
    f->end = &f->contents[length];
    populate_lines(f);
    if (files != NULL) {
        f->next = *files;
        *files = f;
    }
    return f;
}

//
// Create a virtual file from a string.
//
file_t *spoof_file(file_t **files, const char *filename, const char *text)
{
    if (filename == NULL) filename = "";
    file_t *f = new(file_t);
    f->filename = strdup(filename);
    f->contents = strdup(text);
    f->end = &f->contents[strlen(text)];
    populate_lines(f);
    if (files != NULL) {
        f->next = *files;
        *files = f;
    }
    return f;
}

//
// Ensure that the file's contents are held in memory, rather than being memory
// mapped IO.
//
void intern_file(file_t *f)
{
    if (!f->mmapped) return;
    size_t size = (size_t)(f->end - f->contents);
    char *buf = xcalloc(sizeof(char), size + 1);
    memcpy(buf, f->contents, size);
    check(munmap(f->contents, size) == 0,
          "Failure to un-memory-map some memory");
    f->contents = buf;
    f->end = buf + size;
    f->mmapped = 0;
    xfree(&f->lines);
    populate_lines(f);
}

//
// Free a file and all memory contained inside its members, then set the input
// pointer to NULL.
//
void destroy_file(file_t **f)
{
    if ((*f)->filename) {
        xfree(&((*f)->filename));
    }

    if ((*f)->lines) {
        xfree(&((*f)->lines));
    }

    if ((*f)->contents) {
        if ((*f)->mmapped) {
            check(munmap((*f)->contents, (size_t)((*f)->end - (*f)->contents)) == 0,
                  "Failure to un-memory-map some memory");
            (*f)->contents = NULL;
        } else {
            xfree(&((*f)->contents));
        }
    }

    for (allocated_pat_t *next; (*f)->pats; (*f)->pats = next) {
        next = (*f)->pats->next;
        destroy_pat(&(*f)->pats->pat);
        xfree(&(*f)->pats);
    }

    xfree(f);
}

//
// Given a pointer, determine which line number it points to.
//
size_t get_line_number(file_t *f, const char *p)
{
    // TODO: binary search
    for (size_t n = 1; n < f->nlines; n++) {
        if (f->lines[n] > p)
            return n;
    }
    return f->nlines;
}

//
// Given a pointer, determine which character offset within the line it points to.
//
static size_t get_char_number(file_t *f, const char *p)
{
    size_t linenum = get_line_number(f, p);
    return 1 + (size_t)(p - f->lines[linenum-1]);
}

//
// Return a pointer to the line with the specified line number.
//
const char *get_line(file_t *f, size_t line_number)
{
    if (line_number == 0 || line_number > f->nlines) return NULL;
    return f->lines[line_number - 1];
}

//
// Print the filename/line number, followed by the given message, followed by
// the line itself.
//
void fprint_line(FILE *dest, file_t *f, const char *start, const char *end, const char *fmt, ...)
{
    if (start < f->contents) start = f->contents;
    if (start > f->end) start = f->end;
    if (end < f->contents) end = f->contents;
    if (end > f->end) end = f->end;
    size_t linenum = get_line_number(f, start);
    const char *line = get_line(f, linenum);
    size_t charnum = get_char_number(f, start);
    fprintf(dest, "\033[1m%s:%lu:\033[0m ", f->filename[0] ? f->filename : "stdin", linenum);

    va_list args;
    va_start(args, fmt);
    (void)vfprintf(dest, fmt, args);
    va_end(args);
    (void)fputc('\n', dest);

    const char *eol = linenum == f->nlines ? strchr(line, '\0') : strchr(line, '\n');
    if (end == NULL || end > eol) end = eol;
    fprintf(dest, "\033[2m%5lu\033(0\x78\033(B\033[0m%.*s\033[41;30m%.*s\033[0m%.*s\n",
            linenum,
            (int)charnum - 1, line,
            (int)(end - &line[charnum-1]), &line[charnum-1],
            (int)(eol - end - 1), end);
    fprintf(dest, "      \033[34;1m");
    const char *p = line;
    for (; p < start; ++p) (void)fputc(*p == '\t' ? '\t' : ' ', dest);
    if (start == end) ++end;
    for (; p < end; ++p)
        if (*p == '\t')
            // Some janky hacks: 8 ^'s, backtrack 8 spaces, move forward a tab stop, clear any ^'s that overshot
            fprintf(dest, "^^^^^^^^\033[8D\033[I\033[K");
        else
            (void)fputc('^', dest);
    fprintf(dest, "\033[0m\n");
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
