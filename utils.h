/*
 * utils.h - Some helper code for debugging and error logging.
 */

// TODO: better error reporting
#define check(cond, ...) do { if (!(cond)) { fprintf(stderr, __VA_ARGS__); fwrite("\n", 1, 1, stderr); _exit(1); } } while(0)
#define debug(...) do { if (verbose) fprintf(stderr, __VA_ARGS__); } while(0)

/* 
 * Helper function to skip past all spaces (and comments)
 * Returns a pointer to the first non-space character.
 */
static inline const char *after_spaces(const char *str)
{
    // Skip whitespace and comments:
  skip_whitespace:
    switch (*str) {
        case ' ': case '\r': case '\n': case '\t': {
            ++str;
            goto skip_whitespace;
        }
        case '#': {
            while (*str && *str != '\n') ++str;
            goto skip_whitespace;
        }
    }
    return str;
}

static inline int matchchar(const char **str, char c)
{
    *str = after_spaces(*str);
    if (**str == c) {
        ++(*str);
        return 1;
    } else {
        return 0;
    }
}
