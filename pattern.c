//
// pattern.c - Compile strings into BP pattern objects that can be matched against.
//
#include <ctype.h>
#include <err.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "definitions.h"
#include "files.h"
#include "pattern.h"
#include "utils.h"
#include "utf8.h"

__attribute__((nonnull))
static pat_t *expand_replacements(file_t *f, pat_t *replace_pat);
__attribute__((nonnull))
static pat_t *expand_chain(file_t *f, pat_t *first);
__attribute__((nonnull))
static pat_t *expand_choices(file_t *f, pat_t *first);
__attribute__((nonnull))
static pat_t *_bp_simplepattern(file_t *f, const char *str);
__attribute__((nonnull(1,2,3,6)))
static pat_t *new_range(file_t *f, const char *start, const char *end, size_t min, ssize_t max, pat_t *repeating, pat_t *sep);
__attribute__((nonnull(1,2)))
static pat_t *bp_simplepattern(file_t *f, const char *str);

//
// Allocate a new pattern for this file (ensuring it will be automatically
// freed when the file is freed)
//
pat_t *new_pat(file_t *f, const char *start, const char *end, size_t minlen, ssize_t maxlen, enum pattype_e type)
{
    allocated_pat_t *tracker = new(allocated_pat_t);
    tracker->next = f->pats;
    f->pats = tracker;
    tracker->pat.type = type;
    tracker->pat.start = start;
    tracker->pat.end = end;
    tracker->pat.min_matchlen = minlen;
    tracker->pat.max_matchlen = maxlen;
    return &tracker->pat;
}

//
// Helper function to initialize a range object.
//
static pat_t *new_range(file_t *f, const char *start, const char *end, size_t min, ssize_t max, pat_t *repeating, pat_t *sep)
{
    size_t minlen = min*repeating->min_matchlen + (min > 0 ? min-1 : 0)*(sep ? sep->min_matchlen : 0);
    ssize_t maxlen = (max == -1 || UNBOUNDED(repeating) || (max != 0 && max != 1 && sep && UNBOUNDED(sep))) ? -1
        : max*repeating->max_matchlen + (ssize_t)(max > 0 ? min-1 : 0)*(ssize_t)(sep ? sep->min_matchlen : 0);
    pat_t *range = new_pat(f, start, end, minlen, maxlen, BP_REPEAT);
    range->args.repetitions.min = min;
    range->args.repetitions.max = max;
    range->args.repetitions.repeat_pat = repeating;
    range->args.repetitions.sep = sep;
    return range;
}

//
// Take a pattern and expand it into a chain of patterns if it's followed by
// any patterns (e.g. "`x `y"), otherwise return the original input.
//
static pat_t *expand_chain(file_t *f, pat_t *first)
{
    pat_t *second = bp_simplepattern(f, first->end);
    if (second == NULL) return first;
    second = expand_chain(f, second);
    if (second->end <= first->end)
        file_err(f, second->end, second->end,
                 "This chain is not parsing properly");
    return chain_together(f, first, second);
}

//
// Match trailing => replacements (with optional pattern beforehand)
//
static pat_t *expand_replacements(file_t *f, pat_t *replace_pat)
{
    const char *str = replace_pat->end;
    while (matchstr(&str, "=>")) {
        const char *repstr;
        size_t replen;
        if (matchchar(&str, '"') || matchchar(&str, '\'')) {
            char quote = str[-1];
            repstr = str;
            for (; *str && *str != quote; str = next_char(f, str)) {
                if (*str == '\\') {
                    if (!str[1] || str[1] == '\n')
                        file_err(f, str, str+1,
                                 "There should be an escape sequence after this backslash.");
                    str = next_char(f, str);
                }
            }
            replen = (size_t)(str-repstr);
            (void)matchchar(&str, quote);
        } else {
            repstr = "";
            replen = 0;
        }

        pat_t *pat = new_pat(f, replace_pat->start, str, replace_pat->min_matchlen,
                             replace_pat->max_matchlen, BP_REPLACE);
        pat->args.replace.pat = replace_pat;
        pat->args.replace.text = repstr;
        pat->args.replace.len = replen;
        replace_pat = pat;
    }
    return replace_pat;
}

//
// Take a pattern and parse any "=>" replacements and then expand it into a
// chain of choices if it's followed by any "/"-separated patterns (e.g.
// "`x/`y"), otherwise return the original input.
//
static pat_t *expand_choices(file_t *f, pat_t *first)
{
    first = expand_chain(f, first);
    first = expand_replacements(f, first);
    const char *str = first->end;
    if (!matchchar(&str, '/')) return first;
    pat_t *second = bp_simplepattern(f, str);
    if (matchstr(&str, "=>"))
        second = expand_replacements(f, second ? second : new_pat(f, str-2, str-2, 0, 0, BP_STRING));
    if (!second)
        file_err(f, str, str, "There should be a pattern here after a '/'");
    second = expand_choices(f, second);
    return either_pat(f, first, second);
}

//
// Given two patterns, return a new pattern for the first pattern followed by
// the second. If either pattern is NULL, return the other.
//
pat_t *chain_together(file_t *f, pat_t *first, pat_t *second)
{
    if (first == NULL) return second;
    if (second == NULL) return first;
    size_t minlen = first->min_matchlen + second->min_matchlen;
    ssize_t maxlen = (UNBOUNDED(first) || UNBOUNDED(second)) ? -1 : first->max_matchlen + second->max_matchlen;
    pat_t *chain = new_pat(f, first->start, second->end, minlen, maxlen, BP_CHAIN);
    chain->args.multiple.first = first;
    chain->args.multiple.second = second;

    // If `first` is an UPTO operator (..) or contains one, then let it know
    // that `second` is what it's up *to*.
    for (pat_t *p = first; p; ) {
        if (p->type == BP_UPTO) {
            p->args.multiple.first = second;
            p->min_matchlen = second->min_matchlen;
            p->max_matchlen = -1;
            break;
        } else if (p->type == BP_CAPTURE) {
            p = p->args.capture.capture_pat;
        } else if (p->type == BP_CHAIN) {
            p = p->args.multiple.second;
        } else if (p->type == BP_MATCH || p->type == BP_NOT_MATCH) {
            p = p->args.pat;
        } else break;
    }
    return chain;
}

//
// Given two patterns, return a new pattern for matching either the first
// pattern or the second. If either pattern is NULL, return the other.
//
pat_t *either_pat(file_t *f, pat_t *first, pat_t *second)
{
    if (first == NULL) return second;
    if (second == NULL) return first;
    size_t minlen = first->min_matchlen < second->min_matchlen ? first->min_matchlen : second->min_matchlen;
    ssize_t maxlen = (UNBOUNDED(first) || UNBOUNDED(second)) ? -1 : 
        (first->max_matchlen > second->max_matchlen ? first->max_matchlen : second->max_matchlen);
    pat_t *either = new_pat(f, first->start, second->end, minlen, maxlen, BP_OTHERWISE);
    either->args.multiple.first = first;
    either->args.multiple.second = second;
    return either;
}

//
// Wrapper for _bp_simplepattern() that expands any postfix operators
//
static pat_t *bp_simplepattern(file_t *f, const char *str)
{
    pat_t *pat = _bp_simplepattern(f, str);
    if (pat == NULL) return pat;

    if (pat->end == NULL)
        errx(EXIT_FAILURE, "pat->end is uninitialized!");

    // Expand postfix operators (if any)
    str = after_spaces(pat->end);
    while (str+2 < f->end) {
        enum pattype_e type;
        if (matchchar(&str, '~'))
            type = BP_MATCH;
        else if (matchstr(&str, "!~"))
            type = BP_NOT_MATCH;
        else break;

        pat_t *first = pat;
        pat_t *second = bp_simplepattern(f, str);
        if (!second)
            file_err(f, str, str, "The '%s' operator expects a pattern before and after.", type == BP_MATCH ? "~" : "!~");

        pat = new_pat(f, str, second->end, first->min_matchlen, first->max_matchlen, type);
        pat->args.multiple.first = first;
        pat->args.multiple.second = second;
        str = pat->end;
        str = after_spaces(str);
    }

    return pat;
}

//
// Compile a string of BP code into a BP pattern object.
//
static pat_t *_bp_simplepattern(file_t *f, const char *str)
{
    str = after_spaces(str);
    if (!*str) return NULL;
    const char *start = str;
    char c = *str;
    str = next_char(f, str);
    switch (c) {
        // Any char (dot)
        case '.': {
            if (*str == '.') { // ".."
                pat_t *skip = NULL;
                str = next_char(f, str);
                if (matchchar(&str, '%')) {
                    skip = bp_simplepattern(f, str);
                    if (!skip)
                        file_err(f, str, str, "There should be a pattern to skip here after the '%%'");
                    str = skip->end;
                }
                pat_t *upto = new_pat(f, start, str, 0, -1, BP_UPTO);
                upto->args.multiple.second = skip;
                return upto;
            } else {
                return new_pat(f, start, str, 1, UTF8_MAXCHARLEN, BP_ANYCHAR);
            }
        }
        // Char literals
        case '`': {
            pat_t *all = NULL;
            do {
                if (str >= f->end || !*str || *str == '\n')
                    file_err(f, str, str, "There should be a character here after the '`'");

                const char *c1_loc = str;
                str = next_char(f, c1_loc);
                if (matchchar(&str, '-')) { // Range
                    const char *c2_loc = str;
                    if (next_char(f, c1_loc) > c1_loc+1 || next_char(f, c2_loc) > c2_loc+1)
                        file_err(f, start, next_char(f, c2_loc), "Sorry, UTF-8 character ranges are not yet supported.");
                    char c1 = *c1_loc, c2 = *c2_loc;
                    if (!c2 || c2 == '\n')
                        file_err(f, str, str, "There should be a character here to complete the character range.");
                    if (c1 > c2) { // Swap order
                        char tmp = c1;
                        c1 = c2;
                        c2 = tmp;
                    }
                    str = next_char(f, c2_loc);
                    pat_t *pat = new_pat(f, start == c1_loc - 1 ? start : c1_loc, str, 1, 1, BP_RANGE);
                    pat->args.range.low = (unsigned char)c1;
                    pat->args.range.high = (unsigned char)c2;
                    all = either_pat(f, all, pat);
                } else {
                    size_t len = (size_t)(str - c1_loc);
                    pat_t *pat = new_pat(f, c1_loc, str, len, (ssize_t)len, BP_STRING);
                    pat->args.string = c1_loc;
                    all = either_pat(f, all, pat);
                }
            } while (matchchar(&str, ','));

            return all;
        }
        // Escapes
        case '\\': {
            if (!*str || *str == '\n')
                file_err(f, str, str, "There should be an escape sequence here after this backslash.");

            if (matchchar(&str, 'N')) // \N (nodent)
                return new_pat(f, start, str, 1, -1, BP_NODENT);

            const char *opstart = str;
            unsigned char e = (unsigned char)unescapechar(str, &str);
            if (matchchar(&str, '-')) { // Escape range (e.g. \x00-\xFF)
                if (next_char(f, str) != str+1)
                    file_err(f, start, next_char(f, str), "Sorry, UTF8 escape sequences are not supported.");
                const char *seqstart = str;
                unsigned char e2 = (unsigned char)unescapechar(str, &str);
                if (str == seqstart)
                    file_err(f, seqstart, str+1, "This value isn't a valid escape sequence");
                if (e2 < e)
                    file_err(f, start, str, "Escape ranges should be low-to-high, but this is high-to-low.");
                pat_t *esc = new_pat(f, opstart, str, 1, 1, BP_RANGE);
                esc->args.range.low = e;
                esc->args.range.high = e2;
                return esc;
            } else if (str > opstart) {
                pat_t *esc = new_pat(f, start, str, 1, 1, BP_STRING);
                char *s = xcalloc(sizeof(char), 2);
                s[0] = (char)e;
                esc->args.string = s;
                return esc;
            } else {
                const char *next = next_char(f, opstart);
                size_t len = (size_t)(next-opstart);
                pat_t *esc = new_pat(f, start, next, len, (ssize_t)len, BP_STRING);
                char *s = xcalloc(sizeof(char), 1+len);
                memcpy(s, opstart, len);
                esc->args.string = s;
                return esc;
            }
        }
        // String literal
        case '"': case '\'': case '{': case '\002': {
            char endquote = c == '{' ? '}' : (c == '\002' ? '\003' : c);
            char *litstart = (char*)str;
            while (str < f->end && *str != endquote)
                str = next_char(f, str);
            size_t len = (size_t)(str - litstart);
            str = next_char(f, str);

            pat_t *pat = new_pat(f, start, str, len, (ssize_t)len, BP_STRING);
            pat->args.string = litstart;
            
            if (c == '{') { // Surround with `|` word boundaries
                pat_t *left = new_pat(f, start, start+1, 0, -1, BP_REF);
                left->args.ref.name = "left-word-edge";
                left->args.ref.len = strlen(left->args.ref.name);

                pat_t *right = new_pat(f, str-1, str, 0, -1, BP_REF);
                right->args.ref.name = "right-word-edge";
                right->args.ref.len = strlen(right->args.ref.name);

                pat = chain_together(f, left, chain_together(f, pat, right));
            }
            return pat;
        }
        // Not <pat>
        case '!': {
            pat_t *p = bp_simplepattern(f, str);
            if (!p) file_err(f, str, str, "There should be a pattern after this '!'");
            pat_t *not = new_pat(f, start, p->end, 0, 0, BP_NOT);
            not->args.pat = p;
            return not;
        }
        // Number of repetitions: <N>(-<N> / - / + / "")
        case '0': case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9': {
            size_t min = 0;
            ssize_t max = -1;
            --str;
            long n1 = strtol(str, (char**)&str, 10);
            if (matchchar(&str, '-')) {
                str = after_spaces(str);
                const char *numstart = str;
                long n2 = strtol(str, (char**)&str, 10);
                if (str == numstart) min = 0, max = (ssize_t)n1;
                else min = (size_t)n1, max = (ssize_t)n2;
            } else if (matchchar(&str, '+')) {
                min = (size_t)n1, max = -1;
            } else {
                min = (size_t)n1, max = (ssize_t)n1;
            }
            pat_t *repeating = bp_simplepattern(f, str);
            if (!repeating)
                file_err(f, str, str, "There should be a pattern after this repetition count.");
            str = repeating->end;
            pat_t *sep = NULL;
            if (matchchar(&str, '%')) {
                sep = bp_simplepattern(f, str);
                if (!sep)
                    file_err(f, str, str, "There should be a separator pattern after this '%%'");
                str = sep->end;
            } else {
                str = repeating->end;
            }
            return new_range(f, start, str, min, max, repeating, sep);
        }
        // Lookbehind
        case '<': {
            pat_t *behind = bp_simplepattern(f, str);
            if (!behind)
                file_err(f, str, str, "There should be a pattern after this '<'");
            str = behind->end;
            str = behind->end;
            pat_t *pat = new_pat(f, start, str, 0, 0, BP_AFTER);
            pat->args.pat = behind;
            return pat;
        }
        // Lookahead
        case '>': {
            pat_t *ahead = bp_simplepattern(f, str);
            if (!ahead)
                file_err(f, str, str, "There should be a pattern after this '>'");
            str = ahead->end;
            pat_t *pat = new_pat(f, start, str, 0, 0, BP_BEFORE);
            pat->args.pat = ahead;
            return pat;
        }
        // Parentheses
        case '(': {
            if (matchstr(&str, "!)")) { // (!) errors
                pat_t *pat = bp_simplepattern(f, str);
                if (!pat) pat = new_pat(f, str, str, 0, 0, BP_STRING);
                pat = expand_replacements(f, pat);
                pat_t *error = new_pat(f, start, pat->end, pat->min_matchlen, pat->max_matchlen, BP_ERROR);
                error->args.pat = pat;
                return error;
            }

            pat_t *pat = bp_pattern(f, str);
            if (!pat)
                file_err(f, str, str, "There should be a valid pattern after this parenthesis.");
            str = pat->end;
            (void)matchchar(&str, ')');
            pat->start = start;
            pat->end = str;
            return pat;
        }
        // Square brackets
        case '[': {
            pat_t *maybe = bp_pattern(f, str);
            if (!maybe)
                file_err(f, str, str, "There should be a valid pattern after this square bracket.");
            str = maybe->end;
            (void)matchchar(&str, ']');
            return new_range(f, start, str, 0, 1, maybe, NULL);
        }
        // Repeating
        case '*': case '+': {
            size_t min = (size_t)(c == '*' ? 0 : 1);
            pat_t *repeating = bp_simplepattern(f, str);
            if (!repeating)
                file_err(f, str, str, "There should be a valid pattern here after the '%c'", c);
            str = repeating->end;
            pat_t *sep = NULL;
            if (matchchar(&str, '%')) {
                sep = bp_simplepattern(f, str);
                if (!sep)
                    file_err(f, str, str, "There should be a separator pattern after the '%%' here.");
                str = sep->end;
            }
            return new_range(f, start, str, min, -1, repeating, sep);
        }
        // Capture
        case '@': {
            const char *name = NULL;
            size_t namelen = 0;
            const char *a = after_name(str);
            if (a > str && after_spaces(a)[0] == '=' && after_spaces(a)[1] != '>') {
                name = str;
                namelen = (size_t)(a-str);
                str = after_spaces(a) + 1;
            }
            pat_t *pat = bp_simplepattern(f, str);
            if (!pat)
                file_err(f, str, str, "There should be a valid pattern here to capture after the '@'");

            pat_t *capture = new_pat(f, start, pat->end, pat->min_matchlen, pat->max_matchlen, BP_CAPTURE);
            capture->args.capture.capture_pat = pat;
            capture->args.capture.name = name;
            capture->args.capture.namelen = namelen;
            return capture;
        }
        // Start of file/line:
        case '^': {
            if (matchchar(&str, '^'))
                return new_pat(f, start, str, 0, 0, BP_START_OF_FILE);
            return new_pat(f, start, str, 0, 0, BP_START_OF_LINE);
        }
        // End of file/line:
        case '$': {
            if (matchchar(&str, '$'))
                return new_pat(f, start, str, 0, 0, BP_END_OF_FILE);
            return new_pat(f, start, str, 0, 0, BP_END_OF_LINE);
        }
        // Whitespace:
        case '_': {
            size_t namelen = 1;
            if (matchchar(&str, '_')) // double __ (whitespace with newlines)
                ++namelen;
            if (matchchar(&str, ':')) return NULL; // Don't match definitions
            pat_t *ref = new_pat(f, start, str, 0, -1, BP_REF);
            ref->args.ref.name = start;
            ref->args.ref.len = namelen;
            return ref;
        }
        default: {
            // Reference
            if (!isalpha(c)) return NULL;
            --str;
            const char *refname = str;
            str = after_name(str);
            if (matchchar(&str, ':')) // Don't match definitions
                return NULL;
            pat_t *ref = new_pat(f, start, str, 0, -1, BP_REF);
            ref->args.ref.name = refname;
            ref->args.ref.len = (size_t)(str - refname);
            return ref;
        }
    }
}

//
// Similar to bp_simplepattern, except that the pattern begins with an implicit, unclosable quote.
//
pat_t *bp_stringpattern(file_t *f, const char *str)
{
    pat_t *ret = NULL;
    while (*str) {
        char *start = (char*)str;
        pat_t *interp = NULL;
        for (; str < f->end; str = next_char(f, str)) {
            if (*str == '\\' && str+1 < f->end) {
                interp = bp_simplepattern(f, str + 1);
                if (interp) break;
                // If there is no interpolated value, this is just a plain ol' regular backslash
            }
        }
        // End of string
        size_t len = (size_t)(str - start);
        if (len > 0) {
            pat_t *str_chunk = new_pat(f, start, str, len, (ssize_t)len, BP_STRING);
            str_chunk->args.string = start;
            ret = chain_together(f, ret, str_chunk);
        }
        if (interp) {
            ret = chain_together(f, ret, interp);
            str = interp->end;
            // allow terminating seq
            (void)matchchar(&str, ';');
        }
    }
    return ret;
}

//
// Given a pattern and a replacement string, compile the two into a BP
// replace pattern.
//
pat_t *bp_replacement(file_t *f, pat_t *replacepat, const char *replacement)
{
    pat_t *pat = new_pat(f, replacepat->start, replacepat->end, replacepat->min_matchlen, replacepat->max_matchlen, BP_REPLACE);
    pat->args.replace.pat = replacepat;
    const char *p = replacement;
    for (; *p; p++) {
        if (*p == '\\') {
            if (!p[1] || p[1] == '\n')
                file_err(f, p, p, "There should be an escape sequence or pattern here after this backslash.");
            ++p;
        }
    }
    size_t rlen = (size_t)(p-replacement);
    char *rcpy = xcalloc(sizeof(char), rlen + 1);
    memcpy(rcpy, replacement, rlen);
    pat->args.replace.text = rcpy;
    pat->args.replace.len = rlen;
    return pat;
}

//
// Compile a string representing a BP pattern into a pattern object.
//
pat_t *bp_pattern(file_t *f, const char *str)
{
    pat_t *pat = bp_simplepattern(f, str);
    if (pat != NULL) pat = expand_choices(f, pat);
    if (matchstr(&str, "=>"))
        pat = expand_replacements(f, pat ? pat : new_pat(f, str-2, str-2, 0, 0, BP_STRING));
    return pat;
}

//
// Match a definition (id__`:__pattern)
//
def_t *bp_definition(def_t *defs, file_t *f, const char *str)
{
    const char *name = after_spaces(str);
    str = after_name(name);
    if (!str) return NULL;
    size_t namelen = (size_t)(str - name);
    if (!matchchar(&str, ':')) return NULL;
    pat_t *defpat = bp_pattern(f, str);
    if (!defpat) return NULL;
    (void)matchchar(&defpat->end, ';'); // TODO: verify this is safe to mutate
    return with_def(defs, namelen, name, defpat);
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
