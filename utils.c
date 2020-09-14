/*
 * utils.c - Some helper code for debugging and error logging.
 */
#include "utils.h"

/* 
 * Helper function to skip past all spaces (and comments)
 * Returns a pointer to the first non-space character.
 */
const char *after_spaces(const char *str)
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

/*
 * Return the first character after a valid BPEG name, or NULL if none is
 * found.
 */
const char *after_name(const char *str)
{
    if (*str == '^' || *str == '_' || *str == '$') {
        return (str[1] == *str) ? &str[2] : &str[1];
    }
    if (!isalpha(*str)) return NULL;
    for (++str; *str; ++str) {
        if (!(isalnum(*str) || *str == '-'))
            break;
    }
    return str;
}

/*
 * Check if a character is found and if so, move past it.
 */
int matchchar(const char **str, char c)
{
    const char *next = after_spaces(*str);
    if (*next == c) {
        *str = &next[1];
        return 1;
    } else {
        return 0;
    }
}

/*
 * Process a string escape sequence for a character and return the
 * character that was escaped.
 * Set *end = the first character past the end of the escape sequence.
 */
char unescapechar(const char *escaped, const char **end)
{
    size_t len = 1;
    char ret = *escaped;
    switch (*escaped) {
        case 'a': ret = '\a'; break; case 'b': ret = '\b'; break;
        case 'n': ret = '\n'; break; case 'r': ret = '\r'; break;
        case 't': ret = '\t'; break; case 'v': ret = '\v'; break;
        case 'e': ret = '\033'; break;
        case 'x': { // Hex
            static const char hextable[255] = {
                ['0']=0x10, ['1']=0x1, ['2']=0x2, ['3']=0x3, ['4']=0x4,
                ['5']=0x5, ['6']=0x6, ['7']=0x7, ['8']=0x8, ['9']=0x9,
                ['a']=0xa, ['b']=0xb, ['c']=0xc, ['d']=0xd, ['e']=0xe, ['f']=0xf,
                ['A']=0xa, ['B']=0xb, ['C']=0xc, ['D']=0xd, ['E']=0xe, ['F']=0xf,
            };
            if (hextable[(int)escaped[1]] && hextable[(int)escaped[2]]) {
                ret = (hextable[(int)escaped[1]] << 4) | (hextable[(int)escaped[2]] & 0xF);
                len = 3;
            }
            break;
        }
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': { // Octal
            ret = escaped[0] - '0';
            if ('0' <= escaped[1] && escaped[1] <= '7') {
                ++len;
                ret = (ret << 3) | (escaped[1] - '0');
                if ('0' <= escaped[2] && escaped[2] <= '7') {
                    ++len;
                    ret = (ret << 3) | (escaped[2] - '0');
                }
            }
            break;
        }
        default: break;
    }
    *end = &escaped[len];
    return ret;
}

/*
 * Write an unescaped version of `src` to `dest` (at most bufsize-1 chars,
 * terminated by a null byte)
 */
size_t unescape_string(char *dest, const char *src, size_t bufsize)
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

/*
 * Read an entire file into memory. (Guaranteeing that ret[-1] == '\0')
 */
char *readfile(int fd)
{
    size_t capacity = 1000, len = 0;
    char *buf = calloc(sizeof(char), capacity+1);
    buf[len++] = '\0';
    ssize_t just_read;
    while ((just_read=read(fd, &buf[len], capacity-len)) > 0) {
        len += (size_t)just_read;
        if (len >= capacity)
            buf = realloc(buf, sizeof(char)*(capacity *= 2));
    }
    buf[len] = '\0';
    close(fd);
    return &buf[1];
}

void freefile(char *f)
{
    free(&f[-1]);
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
