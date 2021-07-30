//
// utils.c - Some helper code for debugging and error logging.
//

#include <ctype.h>
#include <err.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

#include "utils.h"

//
// Helper function to skip past all spaces (and comments)
// Returns a pointer to the first non-space character.
//
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
        default: break;
    }
    return str;
}

//
// Return the first character after a valid BP name, or NULL if none is
// found.
//
const char *after_name(const char *str)
{
    if (*str == '|') return &str[1];
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

//
// Check if a character is found and if so, move past it.
//
bool matchchar(const char **str, char c)
{
    const char *next = after_spaces(*str);
    if (*next == c) {
        *str = &next[1];
        return true;
    }
    return false;
}

//
// Check if a string is found and if so, move past it.
//
bool matchstr(const char **str, const char *target)
{
    const char *next = after_spaces(*str);
    if (strncmp(next, target, strlen(target)) == 0) {
        *str = &next[strlen(target)];
        return true;
    }
    return false;
}

//
// Process a string escape sequence for a character and return the
// character that was escaped.
// Set *end = the first character past the end of the escape sequence.
//
char unescapechar(const char *escaped, const char **end)
{
    size_t len = 1;
    unsigned char ret = (unsigned char)*escaped;
    switch (*escaped) {
        case 'a': ret = '\a'; break; case 'b': ret = '\b'; break;
        case 'n': ret = '\n'; break; case 'r': ret = '\r'; break;
        case 't': ret = '\t'; break; case 'v': ret = '\v'; break;
        case 'e': ret = '\033'; break; case '\\': ret = '\\'; break;
        case 'x': { // Hex
            static const unsigned char hextable[255] = {
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
            ret = (unsigned char)(escaped[0] - '0');
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
        default: {
            if (end) *end = escaped;
            return (char)0;
        }
    }
    if (end) *end = &escaped[len];
    return (char)ret;
}

//
// If the given argument is NULL, print the error message and exit with
// failure. Otherwise return the given argument.
//
void *check_nonnull(void *p, const char *err_msg, ...)
{
    if (p == NULL) {
        va_list args;
        va_start(args, err_msg);
        verr(EXIT_FAILURE, err_msg, args);
        va_end(args);
    }
    return p;
}

//
// Case-insensitive memory comparison
//
int memicmp(const void *v1, const void *v2, size_t n)
{
    int result = 0;
    const char *s1 = (const char*)v1, *s2 = (const char*)v2;
    while (n-- > 0 && (result = tolower(*(s1++)) - tolower(*(s2++))) == 0)
        ;
    return result;
}

//
// Free memory, but also set the pointer to NULL for safety
//
void xfree(void *p)
{
    if (*(void**)p == NULL)
        errx(EXIT_FAILURE, "attempt to free(NULL)");
    free(*(void**)p);
    *((void**)p) = NULL;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
