//
// files.c - Implementation of some file loading functionality.
//

#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files.h"
#include "match.h"
#include "pattern.h"
#include "utils.h"

//
// In the file object, populate the `lines` array with pointers to the
// beginning of each line.
//
__attribute__((nonnull))
static void populate_lines(file_t *f)
{
    // Calculate line numbers:
    size_t linecap = 10;
    f->lines = new(const char*[linecap]);
    f->nlines = 0;
    char *p = f->start;
    for (size_t n = 0; p && p < f->end; ++n) {
        ++f->nlines;
        if (n >= linecap)
            f->lines = grow(f->lines, linecap *= 2);
        f->lines[n] = p;
        do {
            char *nl = strchr(p, '\n');
            if (nl) {
                p = nl+1;
                break;
            } else if (p < f->end)
                p += strlen(p)+1;
        } while (p < f->end);
    }
}

//
// Read an entire file into memory, using a printf-style formatting string to
// construct the filename.
//
file_t *load_filef(file_t **files, const char *fmt, ...)
{
    char filename[PATH_MAX+1] = {'\0'};
    va_list args;
    va_start(args, fmt);
    if (vsnprintf(filename, PATH_MAX, fmt, args) > (int)PATH_MAX)
        errx(EXIT_FAILURE, "File name is too large");
    va_end(args);
    return load_file(files, filename);
}

//
// Read an entire file into memory.
//
file_t *load_file(file_t **files, const char *filename)
{
    int fd = filename[0] == '\0' ? STDIN_FILENO : open(filename, O_RDONLY);
    if (fd < 0) {
        // Check for <file>:<line>
        if (strrchr(filename, ':')) {
            char tmp[PATH_MAX] = {0};
            strcpy(tmp, filename);
            char *colon = strrchr(tmp, ':');
            *colon = '\0';
            file_t *f = load_file(files, tmp);
            if (!f) return f;
            long line = strtol(colon+1, &colon, 10);
            f->start = (char*)get_line(f, (size_t)line);
            f->end = (char*)get_line(f, (size_t)line+1);
            return f;
        }
        return NULL;
    }
    file_t *f = new(file_t);
    f->filename = checked_strdup(filename);

    struct stat sb;
    if (fstat(fd, &sb) == -1)
        goto read_file;

    f->mmapped = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (f->mmapped == MAP_FAILED) {
        f->mmapped = NULL;
        goto read_file;
    }
    f->start = f->mmapped;
    f->end = &f->mmapped[sb.st_size];
    goto finished_loading;

  read_file:
    {
        size_t capacity = 1000, length = 0;
        f->allocated = new(char[capacity]);
        ssize_t just_read;
        while ((just_read=read(fd, &f->allocated[length], (capacity-1) - length)) > 0) {
            length += (size_t)just_read;
            if (length >= capacity-1)
                f->allocated = grow(f->allocated, capacity *= 2);
        }
        f->allocated[length] = '\0';
        f->start = f->allocated;
        f->end = &f->allocated[length];
    }

  finished_loading:
    if (fd != STDIN_FILENO)
        require(close(fd), "Failed to close file");

    populate_lines(f);
    if (files != NULL) {
        f->next = *files;
        *files = f;
    }
    return f;
}

//
// Set a file struct to represent a region of a different file.
//
void slice_file(file_t *slice, file_t *src, const char *start, const char *end)
{
    memset(slice, 0, sizeof(file_t));
    slice->filename = src->filename;
    slice->lines = src->lines;
    slice->nlines = src->nlines;
    slice->start = (char*)start;
    slice->end = (char*)end;
}

//
// Create a virtual file from a string.
//
file_t *spoof_file(file_t **files, const char *filename, const char *text, ssize_t _len)
{
    if (filename == NULL) filename = "";
    file_t *f = new(file_t);
    size_t len = _len == -1 ? strlen(text) : (size_t)_len;
    f->filename = checked_strdup(filename);
    f->allocated = new(char[len+1]);
    memcpy(f->allocated, text, len);
    f->start = &f->allocated[0];
    f->end = &f->allocated[len];
    populate_lines(f);
    if (files != NULL) {
        f->next = *files;
        *files = f;
    }
    return f;
}

//
// Free a file and all memory contained inside its members, then set the input
// pointer to NULL.
//
void destroy_file(file_t **at_f)
{
    file_t *f = (file_t*)*at_f;
    if (f->filename)
        delete(&f->filename);

    if (f->lines)
        delete(&f->lines);

    if (f->allocated)
        delete(&f->allocated);

    if (f->mmapped) {
        require(munmap(f->mmapped, (size_t)(f->end - f->mmapped)),
                          "Failure to un-memory-map some memory");
        f->mmapped = NULL;
    }

    cache_destroy(f);

    for (pat_t *next; f->pats; f->pats = next) {
        next = f->pats->next;
        delete(&f->pats);
    }

    delete(at_f);
}

//
// Given a pointer, determine which line number it points to.
//
size_t get_line_number(file_t *f, const char *p)
{
    if (f->nlines == 0) return 0;
    // Binary search:
    size_t lo = 0, hi = f->nlines-1;
    while (lo <= hi) {
        size_t mid = (lo + hi) / 2;
        if (f->lines[mid] == p)
            return mid + 1;
        else if (f->lines[mid] < p)
            lo = mid + 1;    
        else if (f->lines[mid] > p)
            hi = mid - 1;
    }
    return lo; // Return the line number whose line starts closest before p
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
    if (start < f->start) start = f->start;
    if (start > f->end) start = f->end;
    if (end < f->start) end = f->start;
    if (end > f->end) end = f->end;
    size_t linenum = get_line_number(f, start);
    const char *line = get_line(f, linenum);
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
            (int)(start - line), line,
            (int)(end - start), start,
            (int)(eol - end), end);
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

//
// Hash a string position/pattern.
//
static inline size_t hash(const char *str, pat_t *pat)
{
    return (size_t)str + 2*pat->id;
}

//
// Check if we have memoized a pattern match at the given position for the
// given definitions. If a result has been memoized, set *result to the
// memoized value and return true, otherwise return false.
//
bool cache_get(file_t *f, def_t *defs, const char *str, pat_t *pat, match_t **result)
{
    if (!f->cache.matches) return NULL;
    size_t h = hash(str, pat) & (f->cache.size-1);
    for (match_t *c = f->cache.matches[h]; c; c = c->cache.next) {
        if (c->pat == pat && c->defs_id == (defs?defs->id:0) && c->start == str) {
            // If c->end == NULL, that means no match occurs here
            *result = c->end == NULL ? NULL : c;
            return true;
        }
    }
    return false;
}

//
// Remove an item from the cache.
//
static void cache_remove(file_t *f, match_t *m)
{
    if (!m->cache.home) return;
    *m->cache.home = m->cache.next;
    if (m->cache.next) m->cache.next->cache.home = m->cache.home;
    m->cache.next = NULL;
    m->cache.home = NULL;
    if (--m->refcount == 0) recycle_if_unused(&m);
    --f->cache.occupancy;
}

//
// Save a match in the cache.
//
void cache_save(file_t *f, def_t *defs, const char *str, pat_t *pat, match_t *m)
{
    // As a convention, a match with {.pat=pat, .start=str, .end==NULL} is used
    // to memoize the fact that `pat` will *not* match at `str`.
    if (m == NULL) m = new_match(defs, pat, str, NULL, NULL);

    if (f->cache.occupancy+1 > 3*f->cache.size) {
        if (f->cache.size == MAX_CACHE_SIZE) {
            size_t h = hash(m->start, m->pat) & (f->cache.size-1);
            for (int quota = 2; f->cache.matches[h] && quota > 0; quota--) {
                match_t *last = f->cache.matches[h];
                while (last->cache.next) last = last->cache.next;
                cache_remove(f, last);
            }
        } else {
            match_t **old_matches = f->cache.matches;
            size_t old_size = f->cache.size;
            f->cache.size = old_size == 0 ? 16 : 2*old_size;
            f->cache.matches = new(match_t*[f->cache.size]);

            // Rehash:
            if (old_matches) {
                for (size_t i = 0; i < old_size; i++) {
                    for (match_t *o; (o = old_matches[i]); ) {
                        *o->cache.home = o->cache.next;
                        if (o->cache.next) o->cache.next->cache.home = o->cache.home;
                        size_t h = hash(o->start, o->pat) & (f->cache.size-1);
                        o->cache.home = &(f->cache.matches[h]);
                        o->cache.next = f->cache.matches[h];
                        if (f->cache.matches[h]) f->cache.matches[h]->cache.home = &o->cache.next;
                        f->cache.matches[h] = o;
                    }
                }
                free(old_matches);
            }
        }
    }

    size_t h = hash(m->start, m->pat) & (f->cache.size-1);
    m->cache.home = &(f->cache.matches[h]);
    m->cache.next = f->cache.matches[h];
    if (f->cache.matches[h]) f->cache.matches[h]->cache.home = &m->cache.next;
    f->cache.matches[h] = m;
    ++m->refcount;
    ++f->cache.occupancy;
}

//
// Remove all items from the cache that do not overlap `start` and `end`.
// (This is used to remove useless items from the cache)
//
void cache_prune(file_t *f, const char *start, const char *end)
{
    if (!f->cache.matches) return;
    for (size_t i = 0; i < f->cache.size; i++) {
        for (match_t *m = f->cache.matches[i], *next = NULL; m; m = next) {
            next = m->cache.next;
            if (m->start < start || (m->end ? m->end : m->start) > end)
                cache_remove(f, m);
        }
    }
}

//
// Clear and deallocate the cache.
//
void cache_destroy(file_t *f)
{
    if (!f->cache.matches) return;
    for (size_t i = 0; i < f->cache.size; i++) {
        while (f->cache.matches[i])
            cache_remove(f, f->cache.matches[i]);
    }
    f->cache.occupancy = 0;
    delete(&f->cache.matches);
    f->cache.size = 0;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
