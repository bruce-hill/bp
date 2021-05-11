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
pat_t *new_pat(file_t *f, const char *start, const char *end, ssize_t len, enum pattype_e type)
{
    allocated_pat_t *tracker = new(allocated_pat_t);
    tracker->next = f->pats;
    f->pats = tracker;
    tracker->pat.type = type;
    tracker->pat.start = start;
    tracker->pat.end = end;
    tracker->pat.len = len;
    return &tracker->pat;
}

//
// Helper function to initialize a range object.
//
static pat_t *new_range(file_t *f, const char *start, const char *end, size_t min, ssize_t max, pat_t *repeating, pat_t *sep)
{
    pat_t *range = new_pat(f, start, end, -1, BP_REPEAT);
    if ((ssize_t)min == max && repeating->len >= 0) {
        if (sep == NULL || max == 0)
            range->len = repeating->len * max;
        else if (sep->len >= 0)
            range->len = repeating->len * max + sep->len * (max - 1);
    }
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
// Take a pattern and parse any "=>" replacements and then expand it into a
// chain of choices if it's followed by any "/"-separated patterns (e.g.
// "`x/`y"), otherwise return the original input.
//
static pat_t *expand_choices(file_t *f, pat_t *first)
{
    first = expand_chain(f, first);
    const char *str = first->end;
    
    while (str+2 < f->end && matchstr(&str, "=>")) { // Replacement <pat> => <pat>
        str = after_spaces(str);
        char quote = *str;
        if (!(matchchar(&str, '"') || matchchar(&str, '\'')))
            file_err(f, str, str, "There should be a string literal as a replacement here.");
        const char *repstr = str;
        for (; *str && *str != quote; str++) {
            if (*str == '\\') {
                if (!str[1] || str[1] == '\n')
                    file_err(f, str, str+1,
                             "There should be an escape sequence after this backslash.");
                ++str;
            }
        }
        if (!matchchar(&str, quote))
            file_err(f, &repstr[-1], str, "This string doesn't have a closing quote.");

        pat_t *replacepat = first;
        first = new_pat(f, replacepat->start, str, replacepat->len, BP_REPLACE);
        first->args.replace.pat = replacepat;
        first->args.replace.text = repstr;
        first->args.replace.len = (size_t)(str-repstr-1);
    }

    if (!matchchar(&str, '/')) return first;
    pat_t *second = bp_simplepattern(f, str);
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
    ssize_t len = (first->len >= 0 && second->len >= 0) ? first->len + second->len : -1;
    pat_t *chain = new_pat(f, first->start, second->end, len, BP_CHAIN);
    chain->args.multiple.first = first;
    chain->args.multiple.second = second;

    // If `first` is an UPTO operator (..) or contains one, then let it know
    // that `second` is what it's up *to*.
    for (pat_t *p = first; p; ) {
        if (p->type == BP_UPTO) {
            p->args.multiple.first = second;
            break;
        } else if (p->type == BP_CAPTURE) {
            p = p->args.capture.capture_pat;
        } else if (p->type == BP_CHAIN) {
            p = p->args.multiple.second;
        } else if (p->type == BP_EQUAL || p->type == BP_NOT_EQUAL) {
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
    ssize_t len = first->len == second->len ? first->len : -1;
    pat_t *either = new_pat(f, first->start, second->end, len, BP_OTHERWISE);
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
    while (str+2 < f->end && (matchstr(&str, "!=") || matchstr(&str, "=="))) { // Equality <pat1>==<pat2> and inequality <pat1>!=<pat2>
        bool equal = str[-2] == '=';
        pat_t *first = pat;
        pat_t *second = bp_simplepattern(f, str);
        if (!second)
            file_err(f, str, str, "The '%c=' operator expects a pattern before and after.", equal?'=':'!');
        if (equal) {
            if (!(first->len == -1 || second->len == -1 || first->len == second->len))
                file_err(f, pat->start, second->end,
                  "These two patterns cannot possibly give the same result (different lengths: %ld != %ld)",
                  first->len, second->len);
        }
        pat = new_pat(f, str, second->end, first->len, equal ? BP_EQUAL : BP_NOT_EQUAL);
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
    ++str;
    switch (c) {
        // Any char (dot)
        case '.': {
            if (*str == '.') { // ".."
                pat_t *skip = NULL;
                ++str;
                if (matchchar(&str, '%')) {
                    skip = bp_simplepattern(f, str);
                    if (!skip)
                        file_err(f, str, str, "There should be a pattern to skip here after the '%%'");
                    str = skip->end;
                }
                pat_t *upto = new_pat(f, start, str, -1, BP_UPTO);
                upto->args.multiple.second = skip;
                return upto;
            } else {
                return new_pat(f, start, str, 1, BP_ANYCHAR);
            }
        }
        // Char literals
        case '`': {
            pat_t *all = NULL;
            do {
                const char *charloc = str;
                c = *str;
                if (!c || c == '\n')
                    file_err(f, str, str, "There should be a character here after the '`'");
                const char *opstart = str-1;

                ++str;
                if (matchchar(&str, '-')) { // Range
                    char c2 = *str;
                    if (!c2 || c2 == '\n')
                        file_err(f, str, str, "There should be a character here to complete the character range.");
                    if (c > c2) { // Swap order
                        char tmp = c;
                        c = c2;
                        c2 = tmp;
                    }
                    ++str;
                    pat_t *pat = new_pat(f, opstart, str, 1, BP_RANGE);
                    pat->args.range.low = (unsigned char)c;
                    pat->args.range.high = (unsigned char)c2;
                    all = either_pat(f, all, pat);
                } else {
                    pat_t *pat = new_pat(f, opstart, str, 1, BP_STRING);
                    pat->args.string = charloc;
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
                return new_pat(f, start, str, -1, BP_NODENT);

            const char *opstart = str;
            unsigned char e = (unsigned char)unescapechar(str, &str);
            if (*str == '-') { // Escape range (e.g. \x00-\xFF)
                ++str;
                const char *seqstart = str;
                unsigned char e2 = (unsigned char)unescapechar(str, &str);
                if (str == seqstart)
                    file_err(f, seqstart, str+1, "This value isn't a valid escape sequence");
                if (e2 < e)
                    file_err(f, start, str, "Escape ranges should be low-to-high, but this is high-to-low.");
                pat_t *esc = new_pat(f, opstart, str, 1, BP_RANGE);
                esc->args.range.low = e;
                esc->args.range.high = e2;
                return esc;
            } else {
                pat_t *esc = new_pat(f, opstart, str, 1, BP_STRING);
                char *s = xcalloc(sizeof(char), 2);
                s[0] = (char)e;
                esc->args.string = s;
                return esc;
            }
        }
        // String literal
        case '"': case '\'': case '{': case '\002': {
            char endquote = c == '{' ? '}' : (c == '\002' ? '\003' : c);
            char *litstart = (char*)str;
            for (; *str != endquote; ++str)
                if (str >= f->end)
                    file_err(f, start, str, "This string doesn't have a closing %c.", endquote);
            ssize_t len = (ssize_t)(str - litstart);
            ++str;

            pat_t *pat = new_pat(f, start, str, len, BP_STRING);
            pat->args.string = litstart;
            
            if (c == '{') { // Surround with `|` word boundaries
                pat_t *left = new_pat(f, start, start+1, -1, BP_REF);
                left->args.ref.name = "left-word-boundary";
                left->args.ref.len = strlen(left->args.ref.name);

                pat_t *right = new_pat(f, str, str+1, -1, BP_REF);
                right->args.ref.name = "right-word-boundary";
                right->args.ref.len = strlen(right->args.ref.name);

                pat = chain_together(f, left, chain_together(f, pat, right));
            }
            return pat;
        }
        // Not <pat>
        case '!': {
            pat_t *p = bp_simplepattern(f, str);
            if (!p) file_err(f, str, str, "There should be a pattern after this '!'");
            pat_t *not = new_pat(f, start, p->end, 0, BP_NOT);
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
            if (behind->len == -1)
                file_err(f, start, behind->end,
                         "Sorry, variable-length lookbehind patterns like this are not supported.\n"
                         "Please use a fixed-length lookbehind pattern instead.");
            str = behind->end;
            pat_t *pat = new_pat(f, start, str, 0, BP_AFTER);
            pat->args.pat = behind;
            return pat;
        }
        // Lookahead
        case '>': {
            pat_t *ahead = bp_simplepattern(f, str);
            if (!ahead)
                file_err(f, str, str, "There should be a pattern after this '>'");
            str = ahead->end;
            pat_t *pat = new_pat(f, start, str, 0, BP_BEFORE);
            pat->args.pat = ahead;
            return pat;
        }
        // Parentheses
        case '(': {
            pat_t *pat = bp_simplepattern(f, str);
            if (!pat)
                file_err(f, str, str, "There should be a valid pattern after this parenthesis.");
            pat = expand_choices(f, pat);
            str = pat->end;
            if (!matchchar(&str, ')'))
                file_err(f, start, str, "This parenthesis group isn't properly closed.");
            pat->start = start;
            pat->end = str;
            return pat;
        }
        // Square brackets
        case '[': {
            pat_t *maybe = bp_simplepattern(f, str);
            if (!maybe)
                file_err(f, str, str, "There should be a valid pattern after this square bracket.");
            maybe = expand_choices(f, maybe);
            str = maybe->end;
            if (!matchchar(&str, ']'))
                file_err(f, start, str, "This square bracket group isn't properly closed.");
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
            const char *a = *str == '!' ? &str[1] : after_name(str);
            if (a > str && after_spaces(a)[0] == '=' && after_spaces(a)[1] != '>') {
                name = str;
                namelen = (size_t)(a-str);
                str = after_spaces(a) + 1;
            }
            pat_t *pat = bp_simplepattern(f, str);
            if (!pat)
                file_err(f, str, str, "There should be a valid pattern here to capture after the '@'");

            pat_t *capture = new_pat(f, start, pat->end, pat->len, BP_CAPTURE);
            capture->args.capture.capture_pat = pat;
            capture->args.capture.name = name;
            capture->args.capture.namelen = namelen;
            return capture;
        }
        // Start of file/line:
        case '^': {
            if (matchchar(&str, '^'))
                return new_pat(f, start, str, 0, BP_START_OF_FILE);
            return new_pat(f, start, str, 0, BP_START_OF_LINE);
        }
        // End of file/line:
        case '$': {
            if (matchchar(&str, '$'))
                return new_pat(f, start, str, 0, BP_END_OF_FILE);
            return new_pat(f, start, str, 0, BP_END_OF_LINE);
        }
        // Whitespace:
        case '_': {
            size_t namelen = 1;
            if (matchchar(&str, '_')) // double __ (whitespace with newlines)
                ++namelen;
            if (matchchar(&str, ':')) return NULL; // Don't match definitions
            pat_t *ref = new_pat(f, start, str, -1, BP_REF);
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
            pat_t *ref = new_pat(f, start, str, -1, BP_REF);
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
        for (; str < f->end; str++) {
            if (*str == '\\' && str+1 < f->end) {
                char e = unescapechar(&str[1], NULL);
                // If there is not a special escape sequence (\n, \x0A, \N,
                // etc.) or \\, then check for an interpolated value:
                if (e != str[1] || e == '\\' || e == 'N') {
                    interp = bp_simplepattern(f, str);
                    if (!interp)
                        errx(EXIT_FAILURE, "Failed to match pattern %.*s", 2, str);
                    break;
                } else {
                    interp = bp_simplepattern(f, str + 1);
                    if (interp) break;
                    // If there is no interpolated value, this is just a plain ol' regular backslash
                }
            }
        }
        // End of string
        ssize_t len = (ssize_t)(str - start);
        if (len > 0) {
            pat_t *str_chunk = new_pat(f, start, str, len, BP_STRING);
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
    pat_t *pat = new_pat(f, replacepat->start, replacepat->end, replacepat->len, BP_REPLACE);
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
