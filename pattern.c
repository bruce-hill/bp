//
// pattern.c - Compile strings into BP pattern objects that can be matched against.
//

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pattern.h"
#include "utils.h"

#define file_err(f, ...) do { fprint_line(stderr, f, __VA_ARGS__); exit(1); } while(0)

__attribute__((nonnull))
static pat_t *expand_chain(file_t *f, pat_t *first);
__attribute__((nonnull))
static pat_t *expand_choices(file_t *f, pat_t *first);
__attribute__((nonnull))
static pat_t *_bp_simplepattern(file_t *f, const char *str);
__attribute__((nonnull(1)))
static pat_t *chain_together(file_t *f,pat_t *first, pat_t *second);
__attribute__((nonnull(1,2,3,6)))
static pat_t *new_range(file_t *f, const char *start, const char *end, ssize_t min, ssize_t max, pat_t *repeating, pat_t *sep);

//
// Allocate a new pattern for this file (ensuring it will be automatically
// freed when the file is freed)
//
pat_t *new_pat(file_t *f, const char *start, enum pattype_e type)
{
    allocated_pat_t *tracker = new(allocated_pat_t);
    tracker->next = f->pats;
    f->pats = tracker;
    tracker->pat.type = type;
    tracker->pat.start = start;
    tracker->pat.len = -1;
    return &tracker->pat;
}

//
// Helper function to initialize a range object.
//
static pat_t *new_range(file_t *f, const char *start, const char *end, ssize_t min, ssize_t max, pat_t *repeating, pat_t *sep)
{
    pat_t *range = new_pat(f, start, VM_REPEAT);
    if (repeating->len >= 0 && (sep == NULL || sep->len >= 0) && min == max && min >= 0)
        range->len = repeating->len * min + (sep == NULL || min == 0 ? 0 : sep->len * (min-1));
    else
        range->len = -1;
    range->args.repetitions.min = min;
    range->args.repetitions.max = max;
    range->args.repetitions.repeat_pat = repeating;
    range->args.repetitions.sep = sep;
    if (!range->start) range->start = repeating->start;
    if (!range->end) range->end = repeating->end;
    if (sep) {
        if (sep->start < range->start) range->start = sep->start;
        if (sep->end > range->end) range->end = sep->end;
    }
    range->end = end;
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

        size_t replace_len = (size_t)(str-repstr-1);
        const char *replacement = xcalloc(sizeof(char), replace_len+1);
        memcpy((void*)replacement, repstr, replace_len);
        
        pat_t *replacepat = first;
        first = new_pat(f, replacepat->start, VM_REPLACE);
        first->args.replace.pat = replacepat;
        first->args.replace.text = replacement;
        first->args.replace.len = replace_len;
        first->len = replacepat->len;
        first->end = str;
    }

    if (!matchchar(&str, '/')) return first;
    pat_t *second = bp_simplepattern(f, str);
    if (!second)
        file_err(f, str, str, "There should be a pattern here after a '/'");
    second = expand_choices(f, second);
    pat_t *choice = new_pat(f, first->start, VM_OTHERWISE);
    if (first->len == second->len)
        choice->len = first->len;
    else choice->len = -1;
    choice->end = second->end;
    choice->args.multiple.first = first;
    choice->args.multiple.second = second;
    return choice;
}

//
// Given two patterns, return a new pattern for the first pattern followed by
// the second. If either pattern is NULL, return the other.
//
static pat_t *chain_together(file_t *f, pat_t *first, pat_t *second)
{
    if (first == NULL) return second;
    if (second == NULL) return first;
    pat_t *chain = new_pat(f, first->start, VM_CHAIN);
    chain->start = first->start;
    if (first->len >= 0 && second->len >= 0)
        chain->len = first->len + second->len;
    else chain->len = -1;
    chain->end = second->end;
    chain->args.multiple.first = first;
    chain->args.multiple.second = second;
    return chain;
}

//
// Wrapper for _bp_simplepattern() that expands any postfix operators
//
pat_t *bp_simplepattern(file_t *f, const char *str)
{
    pat_t *pat = _bp_simplepattern(f, str);
    if (pat == NULL) return pat;

    check(pat->end != NULL, "pat->end is uninitialized!");

    // Expand postfix operators (if any)
    str = after_spaces(pat->end);
    while (str+2 < f->end && (matchstr(&str, "!=") || matchstr(&str, "=="))) { // Equality <pat1>==<pat2> and inequality <pat1>!=<pat2>
        int equal = str[-2] == '=';
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
        pat = new_pat(f, str, equal ? VM_EQUAL : VM_NOT_EQUAL);
        pat->end = second->end;
        pat->len = first->len != -1 ? first->len : second->len;
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
                pat_t *upto = new_pat(f, start, VM_UPTO_AND);
                ++str;
                pat_t *till = bp_simplepattern(f, str);
                upto->args.multiple.first = till;
                if (till)
                    str = till->end;
                if (matchchar(&str, '%')) {
                    pat_t *skip = bp_simplepattern(f, str);
                    if (!skip)
                        file_err(f, str, str, "There should be a pattern to skip here after the '%%'");
                    upto->args.multiple.second = skip;
                    str = skip->end;
                }
                upto->end = str;
                return upto;
            } else {
                pat_t *dot = new_pat(f, start, VM_ANYCHAR);
                dot->len = 1;
                dot->end = str;
                return dot;
            }
        }
        // Char literals
        case '`': {
            pat_t *all = NULL;
            do {
                char c = *str;
                if (!c || c == '\n')
                    file_err(f, str, str, "There should be a character here after the '`'");
                const char *opstart = str-1;

                pat_t *pat;
                ++str;
                if (matchchar(&str, '-')) { // Range
                    char c2 = *str;
                    if (!c2 || c2 == '\n')
                        file_err(f, str, str, "There should be a character here to complete the character range.");
                    pat = new_pat(f, opstart, VM_RANGE);
                    if (c < c2) {
                        pat->args.range.low = (unsigned char)c;
                        pat->args.range.high = (unsigned char)c2;
                    } else {
                        pat->args.range.low = (unsigned char)c2;
                        pat->args.range.high = (unsigned char)c;
                    }
                    ++str;
                } else {
                    pat = new_pat(f, opstart, VM_STRING);
                    char *s = xcalloc(sizeof(char), 2);
                    s[0] = c;
                    pat->args.s = s;
                }

                pat->len = 1;
                pat->end = str;

                if (all == NULL) {
                    all = pat;
                } else {
                    pat_t *either = new_pat(f, all->start, VM_OTHERWISE);
                    either->end = pat->end;
                    either->args.multiple.first = all;
                    either->args.multiple.second = pat;
                    either->len = 1;
                    all = either;
                }
                pat = NULL;
            } while (matchchar(&str, ','));

            return all;
        }
        // Escapes
        case '\\': {
            if (!*str || *str == '\n')
                file_err(f, str, str, "There should be an escape sequence here after this backslash.");

            if (matchchar(&str, 'N')) { // \N (nodent)
                pat_t *nodent = new_pat(f, start, VM_NODENT);
                nodent->end = str;
                return nodent;
            }

            pat_t *esc;
            const char *opstart = str;
            unsigned char e = unescapechar(str, &str);
            if (*str == '-') { // Escape range (e.g. \x00-\xFF)
                ++str;
                const char *seqstart = str;
                unsigned char e2 = unescapechar(str, &str);
                if (str == seqstart)
                    file_err(f, seqstart, str+1, "This value isn't a valid escape sequence");
                if (e2 < e)
                    file_err(f, start, str, "Escape ranges should be low-to-high, but this is high-to-low.");
                esc = new_pat(f, opstart, VM_RANGE);
                esc->args.range.low = e;
                esc->args.range.high = e2;
            } else {
                esc = new_pat(f, opstart, VM_STRING);
                char *s = xcalloc(sizeof(char), 2);
                s[0] = (char)e;
                esc->args.s = s;
            }
            esc->len = 1;
            esc->end = str;
            return esc;
        }
        // String literal
        case '"': case '\'': case '\002': {
            char endquote = c == '\002' ? '\003' : c;
            char *litstart = (char*)str;
            for (; *str && *str != endquote; str++) {
                if (*str == '\\') {
                    if (!str[1] || str[1] == '\n')
                        file_err(f, str, str+1,
                                 "There should be an escape sequence after this backslash.");
                    ++str;
                }
            }
            size_t len = (size_t)(str - litstart);
            char *literal = xcalloc(sizeof(char), len+1);
            memcpy(literal, litstart, len);
            // Note: an unescaped string is guaranteed to be no longer than the
            // escaped string, so this is safe to do inplace.
            len = unescape_string(literal, literal, len);

            pat_t *pat = new_pat(f, start, VM_STRING);
            pat->len = (ssize_t)len;
            pat->args.s = literal;

            if (!matchchar(&str, endquote))
                file_err(f, start, str, "This string doesn't have a closing quote.");

            pat->end = str;
            return pat;
        }
        // Not <pat>
        case '!': {
            pat_t *p = bp_simplepattern(f, str);
            if (!p) file_err(f, str, str, "There should be a pattern after this '!'");
            pat_t *not = new_pat(f, start, VM_NOT);
            not->len = 0;
            not->args.pat = p;
            not->end = p->end;
            return not;
        }
        // Number of repetitions: <N>(-<N> / - / + / "")
        case '0': case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9': {
            ssize_t min = -1, max = -1;
            --str;
            long n1 = strtol(str, (char**)&str, 10);
            if (matchchar(&str, '-')) {
                str = after_spaces(str);
                const char *numstart = str;
                long n2 = strtol(str, (char**)&str, 10);
                if (str == numstart) min = 0, max = n1;
                else min = n1, max = n2;
            } else if (matchchar(&str, '+')) {
                min = n1, max = -1;
            } else {
                min = n1, max = n1;
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
            pat_t *pat = new_pat(f, start, VM_AFTER);
            pat->len = 0;
            pat->args.pat = behind;
            pat->end = str;
            return pat;
        }
        // Lookahead
        case '>': {
            pat_t *ahead = bp_simplepattern(f, str);
            if (!ahead)
                file_err(f, str, str, "There should be a pattern after this '>'");
            str = ahead->end;
            pat_t *pat = new_pat(f, start, VM_BEFORE);
            pat->len = 0;
            pat->args.pat = ahead;
            pat->end = str;
            return pat;
        }
        // Parentheses
        case '(': case '{': {
            char closing = c == '(' ? ')' : '}';
            pat_t *pat = bp_simplepattern(f, str);
            if (!pat)
                file_err(f, str, str, "There should be a valid pattern after this parenthesis.");
            pat = expand_choices(f, pat);
            str = pat->end;
            if (!matchchar(&str, closing))
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
            ssize_t min = c == '*' ? 0 : 1;
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
            pat_t *capture = new_pat(f, start, VM_CAPTURE);
            const char *a = *str == '!' ? &str[1] : after_name(str);
            if (a > str && after_spaces(a)[0] == '=' && after_spaces(a)[1] != '>') {
                capture->args.capture.name = strndup(str, (size_t)(a-str));
                str = after_spaces(a) + 1;
            }
            pat_t *captured = bp_simplepattern(f, str);
            if (!captured)
                file_err(f, str, str, "There should be a valid pattern here to capture after the '@'");
            capture->args.capture.capture_pat = captured;
            capture->len = captured->len;
            capture->end = captured->end;
            return capture;
        }
        // Special rules:
        case '_': case '^': case '$': case '|': {
            const char *name = NULL;
            if (matchchar(&str, c)) { // double __, ^^, $$
                if (matchchar(&str, ':')) return NULL; // Don't match definitions
                char tmp[3] = {c, c, '\0'};
                name = strdup(tmp);
            } else {
                if (matchchar(&str, ':')) return NULL; // Don't match definitions
                name = strndup(&c, 1);
            }
            pat_t *ref = new_pat(f, start, VM_REF);
            ref->args.s = name;
            ref->end = str;
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
            pat_t *ref = new_pat(f, start, VM_REF);
            ref->args.s = strndup(refname, (size_t)(str - refname));
            ref->end = str;
            return ref;
        }
    }
    return NULL;
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
        for (; *str; str++) {
            if (*str == '\\') {
                if (!str[1] || str[1] == '\n')
                    file_err(f, str, str, "There should be an escape sequence or pattern here after this backslash.");
                
                if (matchchar(&str, 'N')) { // \N (nodent)
                    interp = new_pat(f, str-2, VM_NODENT);
                    break;
                }

                const char *after_escape;
                unsigned char e = unescapechar(&str[1], &after_escape);
                if (e != str[1]) {
                    str = after_escape - 1;
                    continue;
                }
                if (str[1] == '\\') {
                    ++str;
                    continue;
                }
                interp = bp_simplepattern(f, str + 1);
                if (interp == NULL)
                    file_err(f, str, str+1, "This isn't a valid escape sequence or pattern.");
                break;
            }
        }
        // End of string
        size_t len = (size_t)(str - start);
        char *literal = xcalloc(sizeof(char), len+1);
        memcpy(literal, start, len);
        // Note: an unescaped string is guaranteed to be no longer than the
        // escaped string, so this is safe to do inplace.
        len = unescape_string(literal, literal, len);
        if (len > 0) {
            pat_t *strop = new_pat(f, str, VM_STRING);
            strop->len = (ssize_t)len;
            strop->args.s = literal;
            strop->end = str;
            ret = chain_together(f, ret, strop);
        }
        if (interp) {
            ret = chain_together(f, ret, interp);
            str = interp->end;
            // allow terminating seq
            matchchar(&str, ';');
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
    pat_t *pat = new_pat(f, replacepat->start, VM_REPLACE);
    pat->end = replacepat->end;
    pat->len = replacepat->len;
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
def_t *bp_definition(file_t *f, const char *str)
{
    const char *name = after_spaces(str);
    str = after_name(name);
    if (!str) return NULL;
    size_t namelen = (size_t)(str - name);
    if (!matchchar(&str, ':')) return NULL;
    pat_t *defpat = bp_pattern(f, str);
    if (!defpat) return NULL;
    matchchar(&defpat->end, ';'); // TODO: verify this is safe to mutate
    def_t *def = new(def_t);
    def->file = f;
    def->namelen = namelen;
    def->name = name;
    def->pat = defpat;
    return def;
}

//
// Deallocate memory referenced inside a pattern struct
//
void destroy_pat(pat_t *pat)
{
    switch (pat->type) {
        case VM_STRING: case VM_REF:
            xfree(&pat->args.s);
            break;
        case VM_CAPTURE:
            if (pat->args.capture.name)
                xfree(&pat->args.capture.name);
            break;
        case VM_REPLACE:
            if (pat->args.replace.text)
                xfree(&pat->args.replace.text);
            break;
        default: break;
    }
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1