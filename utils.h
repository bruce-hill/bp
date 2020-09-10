/*
 * utils.h - Some helper code for debugging and error logging.
 */

#define streq(a, b) (strcmp(a, b) == 0)
// TODO: better error reporting
#define check(cond, ...) do { if (!(cond)) { fprintf(stderr, __VA_ARGS__); fwrite("\n", 1, 1, stderr); _exit(1); } } while(0)
#define debug(...) do { if (verbose) fprintf(stderr, __VA_ARGS__); } while(0)

static int verbose = 0;
static int visualize_delay = -1;

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

static void visualize(const char *source, const char *ptr, const char *msg)
{
    if (!verbose) return;
    fprintf(stderr, "\033[0;1m\r\033[2A\033[K%.*s\033[0;2m%s\033[0m\n",
            (int)(ptr-source), source, ptr);
    fprintf(stderr, "\033[0;1m");
    for (--ptr ; ptr > source; --ptr) putc(' ', stderr);
    fprintf(stderr, "^\033[K\n");
    if (msg)
        fprintf(stderr, "\033[K\033[33;1m%s\033[0m", msg);
    if (visualize_delay > 0)
        usleep(visualize_delay);
}
