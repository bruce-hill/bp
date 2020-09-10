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

static inline const char *after_name(const char *str)
{
    if (!isalpha(*str)) return NULL;
    for (++str; *str; ++str) {
        if (!(isalnum(*str) || *str == '-'))
            break;
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

/*
 * Write an unescaped version of `src` to `dest` (at most bufsize-1 chars,
 * terminated by a null byte)
 */
static size_t unescape_string(char *dest, const char *src, size_t bufsize)
{
    size_t len = 0;
#define PUT(c) do { *(dest++) = (char)(c); ++len; } while (0)
    for ( ; *src && len < bufsize; ++src) {
        if (*src != '\\') {
            PUT(*src);
            continue;
        }
        ++src;
        switch (*src) {
            case 'a': PUT('\a'); break; case 'b': PUT('\b'); break;
            case 'n': PUT('\n'); break; case 'r': PUT('\r'); break;
            case 't': PUT('\t'); break; case 'v': PUT('\v'); break;
            case 'e': PUT('\033'); break;
            case 'x': { // Hex
                static const char hextable[255] = {
                    ['0']=0x10, ['1']=0x1, ['2']=0x2, ['3']=0x3, ['4']=0x4,
                    ['5']=0x5, ['6']=0x6, ['7']=0x7, ['8']=0x8, ['9']=0x9,
                    ['a']=0xa, ['b']=0xb, ['c']=0xc, ['d']=0xd, ['e']=0xe, ['f']=0xf,
                    ['A']=0xa, ['B']=0xb, ['C']=0xc, ['D']=0xd, ['E']=0xe, ['F']=0xf,
                };
                if (hextable[(int)src[1]] && hextable[(int)src[2]]) {
                    PUT((hextable[(int)src[1]] << 4) | (hextable[(int)src[2]] & 0xF));
                    src += 2;
                }
                break;
            }
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': { // Octal
                int c = *src - '0';
                if ('0' <= src[1] && src[1] <= '7') {
                    ++src;
                    c = (c << 3) | (*src - '0');
                    if ('0' <= src[1] && src[1] <= '7' && (c << 3) < 256) {
                        ++src;
                        c = (c << 3) | (*src - '0');
                    }
                }
                PUT(c);
                break;
            }
            default: PUT(*src); break;
        }
    }
    *dest = '\0';
    return len;
#undef PUT
}
