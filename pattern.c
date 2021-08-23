//
// pattern.c - Compile strings into BP pattern objects that can be matched against.
//
#include <ctype.h>
#include <err.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "files.h"
#include "pattern.h"
#include "utils.h"
#include "utf8.h"

__attribute__((nonnull))
static pat_t *bp_pattern_nl(file_t *f, const char *str, bool allow_nl);
__attribute__((nonnull))
static pat_t *expand_replacements(file_t *f, pat_t *replace_pat, bool allow_nl);
__attribute__((nonnull))
static pat_t *expand_chain(file_t *f, pat_t *first, bool allow_nl);
__attribute__((nonnull))
static pat_t *expand_choices(file_t *f, pat_t *first, bool allow_nl);
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
    static size_t next_pat_id = 1;
    pat_t *pat = new(pat_t);
    *pat = (pat_t){
        .next = f->pats,
        .type = type,
        .start = start,
        .end = end,
        .min_matchlen = minlen,
        .max_matchlen = maxlen,
        .id = next_pat_id++,
    };
    f->pats = pat;
    return pat;
}

//
// Helper function to initialize a range object.
//
static pat_t *new_range(file_t *f, const char *start, const char *end, size_t min, ssize_t max, pat_t *repeating, pat_t *sep)
{
    size_t minlen = min*repeating->min_matchlen + (min > 0 ? min-1 : 0)*(sep ? sep->min_matchlen : 0);
    ssize_t maxlen = (max == -1 || UNBOUNDED(repeating) || (max != 0 && max != 1 && sep && UNBOUNDED(sep))) ? (ssize_t)-1
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
static pat_t *expand_chain(file_t *f, pat_t *first, bool allow_nl)
{
    const char *str = after_spaces(first->end, allow_nl);
    pat_t *second = bp_simplepattern(f, str);
    if (second == NULL) return first;
    second = expand_chain(f, second, allow_nl);
    if (second->end <= first->end)
        file_err(f, second->end, second->end,
                 "This chain is not parsing properly");
    return chain_together(f, first, second);
}

//
// Match trailing => replacements (with optional pattern beforehand)
//
static pat_t *expand_replacements(file_t *f, pat_t *replace_pat, bool allow_nl)
{
    const char *str = replace_pat->end;
    while (matchstr(&str, "=>", allow_nl)) {
        const char *repstr;
        size_t replen;
        if (matchchar(&str, '"', allow_nl) || matchchar(&str, '\'', allow_nl)
            || matchchar(&str, '{', allow_nl) || matchchar(&str, '\002', allow_nl)) {
            char closequote = str[-1] == '{' ? '}' : (str[-1] == '\002' ? '\003' : str[-1]);
            repstr = str;
            for (; *str && *str != closequote; str = next_char(f, str)) {
                if (*str == '\\') {
                    if (!str[1] || str[1] == '\n')
                        file_err(f, str, str+1,
                                 "There should be an escape sequence after this backslash.");
                    str = next_char(f, str);
                }
            }
            replen = (size_t)(str-repstr);
            (void)matchchar(&str, closequote, true);
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
static pat_t *expand_choices(file_t *f, pat_t *first, bool allow_nl)
{
    first = expand_chain(f, first, allow_nl);
    first = expand_replacements(f, first, allow_nl);
    const char *str = first->end;
    if (!matchchar(&str, '/', allow_nl)) return first;
    str = after_spaces(str, allow_nl);
    pat_t *second = bp_simplepattern(f, str);
    if (second) str = second->end;
    if (matchstr(&str, "=>", allow_nl))
        second = expand_replacements(f, second ? second : new_pat(f, str-2, str-2, 0, 0, BP_STRING), allow_nl);
    if (!second)
        file_err(f, str, str, "There should be a pattern here after a '/'");
    second = expand_choices(f, second, allow_nl);
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
    ssize_t maxlen = (UNBOUNDED(first) || UNBOUNDED(second)) ? (ssize_t)-1 : first->max_matchlen + second->max_matchlen;
    pat_t *chain = new_pat(f, first->start, second->end, minlen, maxlen, BP_CHAIN);
    chain->args.multiple.first = first;
    chain->args.multiple.second = second;

    // If `first` is an UPTO operator (..) or contains one, then let it know
    // that `second` is what it's up *to*.
    for (pat_t *p = first; p; ) {
        if (p->type == BP_UPTO || p->type == BP_UPTO_STRICT) {
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
    ssize_t maxlen = (UNBOUNDED(first) || UNBOUNDED(second)) ? (ssize_t)-1 : 
        (first->max_matchlen > second->max_matchlen ? first->max_matchlen : second->max_matchlen);
    pat_t *either = new_pat(f, first->start, second->end, minlen, maxlen, BP_OTHERWISE);
    either->args.multiple.first = first;
    either->args.multiple.second = second;
    return either;
}

//
// Wrapper for _bp_simplepattern() that expands any postfix operators (~, !~)
//
static pat_t *bp_simplepattern(file_t *f, const char *str)
{
    pat_t *pat = _bp_simplepattern(f, str);
    if (pat == NULL) return pat;
    str = pat->end;

    // Expand postfix operators (if any)
    while (str < f->end) {
        enum pattype_e type;
        if (matchchar(&str, '~', false))
            type = BP_MATCH;
        else if (matchstr(&str, "!~", false))
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
    }

    return pat;
}

//
// Compile a string of BP code into a BP pattern object.
//
static pat_t *_bp_simplepattern(file_t *f, const char *str)
{
    str = after_spaces(str, false);
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
                char skipper = *str;
                if (matchchar(&str, '%', false) || matchchar(&str, '=', false)) {
                    skip = bp_simplepattern(f, str);
                    if (!skip)
                        file_err(f, str, str, "There should be a pattern to skip here after the '%c'", skipper);
                    str = skip->end;
                }
                pat_t *upto = new_pat(f, start, str, 0, -1, skipper == '=' ? BP_UPTO_STRICT : BP_UPTO);
                upto->args.multiple.second = skip;
                return upto;
            } else {
                return new_pat(f, start, str, 1, UTF8_MAXCHARLEN, BP_ANYCHAR);
            }
        }
        // Char literals
        case '`': {
            pat_t *all = NULL;
            do { // Comma-separated items:
                if (str >= f->end || !*str || *str == '\n')
                    file_err(f, str, str, "There should be a character here after the '`'");

                const char *c1_loc = str;
                str = next_char(f, c1_loc);
                if (*str == '-') { // Range
                    const char *c2_loc = ++str;
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
                    pat_t *pat = new_pat(f, start, str, len, (ssize_t)len, BP_STRING);
                    pat->args.string = c1_loc;
                    all = either_pat(f, all, pat);
                }
            } while (*str++ == ',');

            return all;
        }
        // Escapes
        case '\\': {
            if (!*str || *str == '\n')
                file_err(f, str, str, "There should be an escape sequence here after this backslash.");

            pat_t *all = NULL;
            do { // Comma-separated items:
                const char *itemstart = str-1;
                if (*str == 'N') { // \N (nodent)
                    all = either_pat(f, all, new_pat(f, itemstart, ++str, 1, -1, BP_NODENT));
                    continue;
                } else if (*str == 'i') { // \i (identifier char)
                    all = either_pat(f, all, new_pat(f, itemstart, ++str, 1, -1, BP_ID_CONTINUE));
                    continue;
                } else if (*str == 'I') { // \I (identifier char, not including numbers)
                    all = either_pat(f, all, new_pat(f, itemstart, ++str, 1, -1, BP_ID_START));
                    continue;
                } else if (*str == 'b') { // \b word boundary
                    all = either_pat(f, all, new_pat(f, itemstart, ++str, 0, 0, BP_WORD_BOUNDARY));
                    continue;
                }

                const char *opstart = str;
                unsigned char e_low = (unsigned char)unescapechar(str, &str);
                if (str == opstart)
                    file_err(f, start, str+1, "This isn't a valid escape sequence.");
                unsigned char e_high = e_low;
                if (*str == '-') { // Escape range (e.g. \x00-\xFF)
                    ++str;
                    if (next_char(f, str) != str+1)
                        file_err(f, start, next_char(f, str), "Sorry, UTF8 escape sequences are not supported in ranges.");
                    const char *seqstart = str;
                    e_high = (unsigned char)unescapechar(str, &str);
                    if (str == seqstart)
                        file_err(f, seqstart, str+1, "This value isn't a valid escape sequence");
                    if (e_high < e_low)
                        file_err(f, start, str, "Escape ranges should be low-to-high, but this is high-to-low.");
                }
                pat_t *esc = new_pat(f, start, str, 1, 1, BP_RANGE);
                esc->args.range.low = e_low;
                esc->args.range.high = e_high;
                all = either_pat(f, all, esc);
            } while (*str++ == ',');

            return all;
        }
        // Word boundary
        case '|': {
            return new_pat(f, start, str, 0, 0, BP_WORD_BOUNDARY);
        }
        // String literal
        case '"': case '\'': case '\002': case '{': {
            char endquote = c == '\002' ? '\003' : (c == '{' ? '}' : c);
            char *litstart = (char*)str;
            while (str < f->end && *str != endquote)
                str = next_char(f, str);
            size_t len = (size_t)(str - litstart);
            str = next_char(f, str);

            pat_t *pat = new_pat(f, start, str, len, (ssize_t)len, BP_STRING);
            pat->args.string = litstart;
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
            if (matchchar(&str, '-', false)) {
                str = after_spaces(str, false);
                const char *numstart = str;
                long n2 = strtol(str, (char**)&str, 10);
                if (str == numstart) min = 0, max = (ssize_t)n1;
                else min = (size_t)n1, max = (ssize_t)n2;
            } else if (matchchar(&str, '+', false)) {
                min = (size_t)n1, max = -1;
            } else {
                min = (size_t)n1, max = (ssize_t)n1;
            }
            pat_t *repeating = bp_simplepattern(f, str);
            if (!repeating)
                file_err(f, str, str, "There should be a pattern after this repetition count.");
            str = repeating->end;
            pat_t *sep = NULL;
            if (matchchar(&str, '%', false)) {
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
            if (start + 2 < f->end && strncmp(start, "(!)", 3) == 0) { // (!) errors
                str = start + 3;
                pat_t *pat = bp_simplepattern(f, str);
                if (!pat) pat = new_pat(f, str, str, 0, 0, BP_STRING);
                pat = expand_replacements(f, pat, false);
                pat_t *error = new_pat(f, start, pat->end, pat->min_matchlen, pat->max_matchlen, BP_ERROR);
                error->args.pat = pat;
                return error;
            }

            pat_t *pat = bp_pattern_nl(f, str, true);
            if (!pat)
                file_err(f, str, str, "There should be a valid pattern after this parenthesis.");
            str = pat->end;
            if (!matchchar(&str, ')', true)) file_err(f, str, str, "Missing paren: )");
            pat->start = start;
            pat->end = str;
            return pat;
        }
        // Square brackets
        case '[': {
            pat_t *maybe = bp_pattern_nl(f, str, true);
            if (!maybe)
                file_err(f, str, str, "There should be a valid pattern after this square bracket.");
            str = maybe->end;
            (void)matchchar(&str, ']', true);
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
            if (matchchar(&str, '%', false)) {
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
            const char *eq = a;
            if (a > str && !matchstr(&eq, "=>", false) && matchchar(&eq, '=', false)) {
                name = str;
                namelen = (size_t)(a-str);
                str = eq;
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
        // Start of file/line
        case '^': {
            if (*str == '^')
                return new_pat(f, start, ++str, 0, 0, BP_START_OF_FILE);
            return new_pat(f, start, str, 0, 0, BP_START_OF_LINE);
        }
        // End of file/line:
        case '$': {
            if (*str == '$')
                return new_pat(f, start, ++str, 0, 0, BP_END_OF_FILE);
            return new_pat(f, start, str, 0, 0, BP_END_OF_LINE);
        }
        default: {
            // Reference
            if (!isalpha(c) && c != '_') return NULL;
            str = after_name(start);
            size_t namelen = (size_t)(str - start);
            if (matchchar(&str, ':', false)) { // Definitions
                pat_t *def = bp_pattern_nl(f, str, false);
                if (!def) file_err(f, str, f->end, "Could not parse this definition.");
                str = def->end;
                (void)matchchar(&str, ';', false); // Optional semicolon
                str = after_spaces(str, true);
                pat_t *pat = bp_pattern_nl(f, str, false);
                if (pat) str = pat->end;
                else pat = def;
                pat_t *ret = new_pat(f, start, str, pat->min_matchlen, pat->max_matchlen, BP_DEFINITION);
                ret->args.def.name = start;
                ret->args.def.namelen = namelen;
                ret->args.def.def = def;
                ret->args.def.pat = pat;
                return ret;
            }
            pat_t *ref = new_pat(f, start, str, 0, -1, BP_REF);
            ref->args.ref.name = start;
            ref->args.ref.len = namelen;
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
                if (str[1] == '\\' || isalnum(str[1]))
                    interp = bp_simplepattern(f, str);
                else
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
            (void)matchchar(&str, ';', false);
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
    char *rcpy = new(char[rlen + 1]);
    memcpy(rcpy, replacement, rlen);
    pat->args.replace.text = rcpy;
    pat->args.replace.len = rlen;
    return pat;
}

static pat_t *bp_pattern_nl(file_t *f, const char *str, bool allow_nl)
{
    str = after_spaces(str, allow_nl);
    pat_t *pat = bp_simplepattern(f, str);
    if (pat != NULL) pat = expand_choices(f, pat, allow_nl);
    if (matchstr(&str, "=>", allow_nl))
        pat = expand_replacements(f, pat ? pat : new_pat(f, str-2, str-2, 0, 0, BP_STRING), allow_nl);
    return pat;
}

//
// Compile a string representing a BP pattern into a pattern object.
//
pat_t *bp_pattern(file_t *f, const char *str)
{
    return bp_pattern_nl(f, str, false);
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
