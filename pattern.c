//
// pattern.c - Compile strings into BP pattern objects that can be matched against.
//
#include <ctype.h>
#include <err.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pattern.h"
#include "utils.h"
#include "utf8.h"

#define Pattern(_tag, _start, _end, _min, _max, ...) allocate_pat((bp_pat_t){.type=_tag, .start=_start, .end=_end, \
                                                              .min_matchlen=_min, .max_matchlen=_max, .__tagged._tag={__VA_ARGS__}})
#define UNBOUNDED(pat) ((pat)->max_matchlen == -1)

static bp_pat_t *allocated_pats = NULL;

__attribute__((nonnull))
static bp_pat_t *bp_pattern_nl(const char *str, const char *end, bool allow_nl);
__attribute__((nonnull))
static bp_pat_t *bp_simplepattern(const char *str, const char *end);

// For error-handling purposes, use setjmp/longjmp to break out of deeply
// recursive function calls when a parse error occurs.
bool is_in_try_catch = false;
static jmp_buf err_jmp;
static maybe_pat_t parse_error = {.success = false};

#define __TRY_PATTERN__ bool was_in_try_catch = is_in_try_catch; \
    if (!is_in_try_catch) { is_in_try_catch = true; if (setjmp(err_jmp)) return parse_error; }
#define __END_TRY_PATTERN__ if (!was_in_try_catch) is_in_try_catch = false;

static inline void parse_err(const char *start, const char *end, const char *msg)
{
    if (!is_in_try_catch) {
        fprintf(stderr, "Parse error: %s\n%.*s\n", msg, (int)(end-start), start);
        exit(1);
    }
    parse_error.value.error.start = start;
    parse_error.value.error.end = end;
    parse_error.value.error.msg = msg;
    longjmp(err_jmp, 1);
}

//
// Allocate a new pattern for this file (ensuring it will be automatically
// freed when the file is freed)
//
public bp_pat_t *allocate_pat(bp_pat_t pat)
{
    static size_t next_pat_id = 1;
    bp_pat_t *allocated = new(bp_pat_t);
    *allocated = pat;
    allocated->home = &allocated_pats;
    allocated->next = allocated_pats;
    allocated->id = next_pat_id++;
    if (allocated_pats) allocated_pats->home = &allocated->next;
    allocated_pats = allocated;
    return allocated;
}

//
// Helper function to initialize a range object.
//
__attribute__((nonnull(1,2,5)))
static bp_pat_t *new_range(const char *start, const char *end, size_t min, ssize_t max, bp_pat_t *repeating, bp_pat_t *sep)
{
    size_t minlen = min*repeating->min_matchlen + (min > 0 ? min-1 : 0)*(sep ? sep->min_matchlen : 0);
    ssize_t maxlen = (max == -1 || UNBOUNDED(repeating) || (max != 0 && max != 1 && sep && UNBOUNDED(sep))) ? (ssize_t)-1
        : max*repeating->max_matchlen + (ssize_t)(max > 0 ? min-1 : 0)*(ssize_t)(sep ? sep->min_matchlen : 0);
    return Pattern(BP_REPEAT, start, end, minlen, maxlen,
                   .min=min, .max=max, .repeat_pat=repeating, .sep=sep);
}

//
// Take a pattern and expand it into a chain of patterns if it's followed by
// any patterns (e.g. "`x `y"), otherwise return the original input.
//
__attribute__((nonnull))
static bp_pat_t *expand_chain(bp_pat_t *first, const char *end, bool allow_nl)
{
    const char *str = after_spaces(first->end, allow_nl, end);
    bp_pat_t *second = bp_simplepattern(str, end);
    if (second == NULL) return first;
    second = expand_chain(second, end, allow_nl);
    return chain_together(first, second);
}

//
// Match trailing => replacements (with optional pattern beforehand)
//
__attribute__((nonnull))
static bp_pat_t *expand_replacements(bp_pat_t *replace_pat, const char *end, bool allow_nl)
{
    const char *str = replace_pat->end;
    while (matchstr(&str, "=>", allow_nl, end)) {
        const char *repstr;
        size_t replen;
        if (matchchar(&str, '"', allow_nl, end) || matchchar(&str, '\'', allow_nl, end)
            || matchchar(&str, '}', allow_nl, end) || matchchar(&str, '\002', allow_nl, end)) {
            char closequote = str[-1] == '}' ? '{' : (str[-1] == '\002' ? '\003' : str[-1]);
            repstr = str;
            for (; str < end && *str != closequote; str = next_char(str, end)) {
                if (*str == '\\') {
                    if (!str[1] || str[1] == '\n')
                        parse_err(str, str+1,
                                 "There should be an escape sequence after this backslash.");
                    str = next_char(str, end);
                }
            }
            replen = (size_t)(str-repstr);
            (void)matchchar(&str, closequote, true, end);
        } else {
            repstr = "";
            replen = 0;
        }

        replace_pat = Pattern(BP_REPLACE, replace_pat->start, str,
                             replace_pat->min_matchlen, replace_pat->max_matchlen,
                             .pat=replace_pat, .text=repstr, .len=replen);
    }
    return replace_pat;
}

//
// Take a pattern and parse any "=>" replacements and then expand it into a
// chain of choices if it's followed by any "/"-separated patterns (e.g.
// "`x/`y"), otherwise return the original input.
//
__attribute__((nonnull))
static bp_pat_t *expand_choices(bp_pat_t *first, const char *end, bool allow_nl)
{
    first = expand_chain(first, end, allow_nl);
    first = expand_replacements(first, end, allow_nl);
    const char *str = first->end;
    if (!matchchar(&str, '/', allow_nl, end)) return first;
    str = after_spaces(str, allow_nl, end);
    bp_pat_t *second = bp_simplepattern(str, end);
    if (second) str = second->end;
    if (matchstr(&str, "=>", allow_nl, end))
        second = expand_replacements(second ? second : Pattern(BP_STRING, str-2, str-2, 0, 0), end, allow_nl);
    if (!second)
        parse_err(str, str, "There should be a pattern here after a '/'");
    second = expand_choices(second, end, allow_nl);
    return either_pat(first, second);
}

//
// Given two patterns, return a new pattern for the first pattern followed by
// the second. If either pattern is NULL, return the other.
//
public bp_pat_t *chain_together(bp_pat_t *first, bp_pat_t *second)
{
    if (first == NULL) return second;
    if (second == NULL) return first;

    if (first->type == BP_STRING && first->max_matchlen == 0) return second;
    if (second->type == BP_STRING && second->max_matchlen == 0) return first;

    if (first->type == BP_DEFINITIONS && second->type == BP_DEFINITIONS) {
        return Pattern(BP_CHAIN, first->start, second->end, second->min_matchlen, second->max_matchlen, .first=first, .second=second);
    }

    size_t minlen = first->min_matchlen + second->min_matchlen;
    ssize_t maxlen = (UNBOUNDED(first) || UNBOUNDED(second)) ? (ssize_t)-1 : first->max_matchlen + second->max_matchlen;
    return Pattern(BP_CHAIN, first->start, second->end, minlen, maxlen, .first=first, .second=second);
}

//
// Given two patterns, return a new pattern for matching either the first
// pattern or the second. If either pattern is NULL, return the other.
//
public bp_pat_t *either_pat(bp_pat_t *first, bp_pat_t *second)
{
    if (first == NULL) return second;
    if (second == NULL) return first;
    size_t minlen = first->min_matchlen < second->min_matchlen ? first->min_matchlen : second->min_matchlen;
    ssize_t maxlen = (UNBOUNDED(first) || UNBOUNDED(second)) ? (ssize_t)-1 : 
        (first->max_matchlen > second->max_matchlen ? first->max_matchlen : second->max_matchlen);
    return Pattern(BP_OTHERWISE, first->start, second->end, minlen, maxlen, .first=first, .second=second);
}

//
// Parse a definition
//
__attribute__((nonnull))
static bp_pat_t *_bp_definition(const char *start, const char *end)
{
    if (start >= end || !(isalpha(*start) || *start == '_')) return NULL;
    const char *str = after_name(start, end);
    size_t namelen = (size_t)(str - start);
    if (!matchchar(&str, ':', false, end)) return NULL;
    bool is_tagged = str < end && *str == ':' && matchchar(&str, ':', false, end);
    bp_pat_t *def = bp_pattern_nl(str, end, false);
    if (!def) parse_err(str, end, "Could not parse this definition.");
    str = def->end;
    (void)matchchar(&str, ';', false, end); // Optional semicolon
    if (is_tagged) { // `id:: foo` means define a rule named `id` that gives captures an `id` tag
        def = Pattern(BP_TAGGED, def->start, def->end, def->min_matchlen, def->max_matchlen,
                      .pat=def, .name=start, .namelen=namelen);
    }
    bp_pat_t *next_def = _bp_definition(after_spaces(str, true, end), end);
    return Pattern(BP_DEFINITIONS, start, next_def ? next_def->end : str, 0, -1,
                   .name=start, .namelen=namelen, .meaning=def, .next_def=next_def);
}

//
// Compile a string of BP code into a BP pattern object.
//
__attribute__((nonnull))
static bp_pat_t *_bp_simplepattern(const char *str, const char *end, bool inside_stringpattern)
{
    str = after_spaces(str, false, end);
    if (!*str) return NULL;
    const char *start = str;
    char c = *str;
    str = next_char(str, end);
    switch (c) {
    // Any char (dot)
    case '.': {
        // As a special case, 3+ dots is parsed as a series of "any char" followed by a single "upto"
        // In other words, `...foo` parses as `(.)(..foo)` instead of `(..(.)) (foo)`
        // This is so that `...` can mean "at least one character upto" instead of "upto any character",
        // which is tautologically the same as matching any single character.
        if (*str == '.' && (str+1 >= end || str[1] != '.')) { // ".."
            str = next_char(str, end);
            enum bp_pattype_e type = BP_UPTO;
            bp_pat_t *extra_arg = NULL;
            if (matchchar(&str, '%', false, end)) {
                extra_arg = bp_simplepattern(str, end);
                if (extra_arg)
                    str = extra_arg->end;
                else
                    parse_err(str, str, "There should be a pattern to skip here after the '%'");
            } else if (matchchar(&str, '=', false, end)) {
                extra_arg = bp_simplepattern(str, end);
                if (extra_arg)
                    str = extra_arg->end;
                else
                    parse_err(str, str, "There should be a pattern here after the '='");
                type = BP_UPTO_STRICT;
            }
            bp_pat_t *target;
            if (inside_stringpattern) {
                target = NULL;
            } else {
                target = bp_simplepattern(str, end);
                // Bugfix: `echo "foo@" | bp '{..}{"@"}'` should be parsed as `.."@"`
                // not '(.."") "@"'
                while (target && target->type == BP_STRING && target->max_matchlen == 0)
                    target = bp_simplepattern(target->end, end);
            }
            return type == BP_UPTO ?
                Pattern(BP_UPTO, start, str, 0, -1, .target=target, .skip=extra_arg)
                : Pattern(BP_UPTO_STRICT, start, str, 0, -1, .target=target, .skip=extra_arg);
        } else {
            return Pattern(BP_ANYCHAR, start, str, 1, UTF8_MAXCHARLEN);
        }
    }
    // Char literals
    case '`': {
        bp_pat_t *all = NULL;
        do { // Comma-separated items:
            if (str >= end || !*str || *str == '\n')
                parse_err(str, str, "There should be a character here after the '`'");

            const char *c1_loc = str;
            str = next_char(c1_loc, end);
            if (*str == '-') { // Range
                const char *c2_loc = ++str;
                if (next_char(c1_loc, end) > c1_loc+1 || next_char(c2_loc, end) > c2_loc+1)
                    parse_err(start, next_char(c2_loc, end), "Sorry, UTF-8 character ranges are not yet supported.");
                char c1 = *c1_loc, c2 = *c2_loc;
                if (!c2 || c2 == '\n')
                    parse_err(str, str, "There should be a character here to complete the character range.");
                if (c1 > c2) { // Swap order
                    char tmp = c1;
                    c1 = c2;
                    c2 = tmp;
                }
                str = next_char(c2_loc, end);
                bp_pat_t *pat = Pattern(BP_RANGE, start == c1_loc - 1 ? start : c1_loc, str, 1, 1, .low=c1, .high=c2);
                all = either_pat(all, pat);
            } else {
                size_t len = (size_t)(str - c1_loc);
                bp_pat_t *pat = Pattern(BP_STRING, start, str, len, (ssize_t)len, .string=strndup(c1_loc, len));
                all = either_pat(all, pat);
            }
        } while (*str++ == ',');

        return all;
    }
    // Escapes
    case '\\': {
        if (!*str || *str == '\n')
            parse_err(str, str, "There should be an escape sequence here after this backslash.");

        bp_pat_t *all = NULL;
        do { // Comma-separated items:
            const char *itemstart = str-1;
            if (*str == 'N') { // \N (nodent)
                all = either_pat(all, Pattern(BP_NODENT, itemstart, ++str, 1, -1));
                continue;
            } else if (*str == 'C') { // \C (current indent)
                all = either_pat(all, Pattern(BP_CURDENT, itemstart, ++str, 1, -1));
                continue;
            } else if (*str == 'i') { // \i (identifier char)
                all = either_pat(all, Pattern(BP_ID_CONTINUE, itemstart, ++str, 1, -1));
                continue;
            } else if (*str == 'I') { // \I (identifier char, not including numbers)
                all = either_pat(all, Pattern(BP_ID_START, itemstart, ++str, 1, -1));
                continue;
            } else if (*str == 'b') { // \b word boundary
                all = either_pat(all, Pattern(BP_WORD_BOUNDARY, itemstart, ++str, 0, 0));
                continue;
            }

            const char *opstart = str;
            unsigned char e_low = (unsigned char)unescapechar(str, &str, end);
            if (str == opstart)
                parse_err(start, str+1, "This isn't a valid escape sequence.");
            unsigned char e_high = e_low;
            if (*str == '-') { // Escape range (e.g. \x00-\xFF)
                ++str;
                if (next_char(str, end) != str+1)
                    parse_err(start, next_char(str, end), "Sorry, UTF8 escape sequences are not supported in ranges.");
                const char *seqstart = str;
                e_high = (unsigned char)unescapechar(str, &str, end);
                if (str == seqstart)
                    parse_err(seqstart, str+1, "This value isn't a valid escape sequence");
                if (e_high < e_low)
                    parse_err(start, str, "Escape ranges should be low-to-high, but this is high-to-low.");
            }
            bp_pat_t *esc = Pattern(BP_RANGE, start, str, 1, 1, .low=e_low, .high=e_high);
            all = either_pat(all, esc);
        } while (*str == ',' && str++ < end);

        return all;
    }
    // Word boundary
    case '|': {
        return Pattern(BP_WORD_BOUNDARY, start, str, 0, 0);
    }
    // String literal
    case '"': case '\'': case '\002': case '}': {
        char endquote = c == '\002' ? '\003' : (c == '}' ? '{' : c);
        char *litstart = (char*)str;
        while (str < end && *str != endquote)
            str = next_char(str, end);
        size_t len = (size_t)(str - litstart);
        str = next_char(str, end);
        if (c == '}') ++start; // Don't include the "}" in the pattern source range
        return Pattern(BP_STRING, start, str, len, (ssize_t)len, .string=strndup(litstart, len));
    }
    // Not <pat>
    case '!': {
        bp_pat_t *p = bp_simplepattern(str, end);
        if (!p) parse_err(str, str, "There should be a pattern after this '!'");
        return Pattern(BP_NOT, start, p->end, 0, 0, .pat=p);
    }
    // Number of repetitions: <N>(-<N> / - / + / "")
    case '0': case '1': case '2': case '3': case '4': case '5':
    case '6': case '7': case '8': case '9': {
        size_t min = 0;
        ssize_t max = -1;
        --str;
        long n1 = strtol(str, (char**)&str, 10);
        if (matchchar(&str, '-', false, end)) {
            str = after_spaces(str, false, end);
            const char *numstart = str;
            long n2 = strtol(str, (char**)&str, 10);
            if (str == numstart) min = 0, max = (ssize_t)n1;
            else min = (size_t)n1, max = (ssize_t)n2;
        } else if (matchchar(&str, '+', false, end)) {
            min = (size_t)n1, max = -1;
        } else {
            min = (size_t)n1, max = (ssize_t)n1;
        }
        bp_pat_t *repeating = bp_simplepattern(str, end);
        if (!repeating)
            parse_err(str, str, "There should be a pattern after this repetition count.");
        str = repeating->end;
        bp_pat_t *sep = NULL;
        if (matchchar(&str, '%', false, end)) {
            sep = bp_simplepattern(str, end);
            if (!sep)
                parse_err(str, str, "There should be a separator pattern after this '%%'");
            str = sep->end;
        } else {
            str = repeating->end;
        }
        return new_range(start, str, min, max, repeating, sep);
    }
    // Lookbehind
    case '<': {
        bp_pat_t *behind = bp_simplepattern(str, end);
        if (!behind)
            parse_err(str, str, "There should be a pattern after this '<'");
        return Pattern(BP_AFTER, start, behind->end, 0, 0, .pat=behind);
    }
    // Lookahead
    case '>': {
        bp_pat_t *ahead = bp_simplepattern(str, end);
        if (!ahead)
            parse_err(str, str, "There should be a pattern after this '>'");
        return Pattern(BP_BEFORE, start, ahead->end, 0, 0, .pat=ahead);
    }
    // Parentheses
    case '(': {
        bp_pat_t *pat = bp_pattern_nl(str, end, true);
        if (!pat)
            parse_err(str, str, "There should be a valid pattern after this parenthesis.");
        str = pat->end;
        if (!matchchar(&str, ')', true, end)) parse_err(str, str, "Missing paren: )");
        pat->start = start;
        pat->end = str;
        return pat;
    }
    // Square brackets
    case '[': {
        bp_pat_t *maybe = bp_pattern_nl(str, end, true);
        if (!maybe)
            parse_err(str, str, "There should be a valid pattern after this square bracket.");
        str = maybe->end;
        (void)matchchar(&str, ']', true, end);
        return new_range(start, str, 0, 1, maybe, NULL);
    }
    // Repeating
    case '*': case '+': {
        size_t min = (size_t)(c == '*' ? 0 : 1);
        bp_pat_t *repeating = bp_simplepattern(str, end);
        if (!repeating)
            parse_err(str, str, "There should be a valid pattern to repeat here");
        str = repeating->end;
        bp_pat_t *sep = NULL;
        if (matchchar(&str, '%', false, end)) {
            sep = bp_simplepattern(str, end);
            if (!sep)
                parse_err(str, str, "There should be a separator pattern after the '%%' here.");
            str = sep->end;
        }
        return new_range(start, str, min, -1, repeating, sep);
    }
    // Capture
    case '@': {
        if (matchchar(&str, ':', false, end)) { // Tagged capture @:Foo=pat
            const char *name = str;
            str = after_name(name, end);
            if (str <= name)
                parse_err(start, str, "There should be an identifier after this '@:'");
            size_t namelen = (size_t)(str - name);
            bp_pat_t *p = NULL;
            if (matchchar(&str, '=', false, end)) {
                p = bp_simplepattern(str, end);
                if (p) str = p->end;
            }
            return Pattern(BP_TAGGED, start, str, p ? p->min_matchlen : 0, p ? p->max_matchlen : 0,
                           .pat=p, .name=name, .namelen=namelen);
        }

        const char *name = NULL;
        size_t namelen = 0;
        const char *a = after_name(str, end);
        const char *eq = a;
        bool backreffable = false;
        if (a > str && matchchar(&eq, ':', false, end)) {
            name = str;
            namelen = (size_t)(a-str);
            str = eq;
            backreffable = true;
        } else if (a > str && !matchstr(&eq, "=>", false, end) && matchchar(&eq, '=', false, end)) {
            name = str;
            namelen = (size_t)(a-str);
            str = eq;
        }
        bp_pat_t *pat = bp_simplepattern(str, end);
        if (!pat)
            parse_err(str, str, "There should be a valid pattern here to capture after the '@'");

        return Pattern(BP_CAPTURE, start, pat->end, pat->min_matchlen, pat->max_matchlen,
                       .pat = pat, .name = name, .namelen = namelen, .backreffable = backreffable);
    }
    // Start of file/line
    case '^': {
        if (*str == '^')
            return Pattern(BP_START_OF_FILE, start, ++str, 0, 0);
        return Pattern(BP_START_OF_LINE, start, str, 0, 0);
    }
    // End of file/line:
    case '$': {
        if (*str == '$')
            return Pattern(BP_END_OF_FILE, start, ++str, 0, 0);
        return Pattern(BP_END_OF_LINE, start, str, 0, 0);
    }
    default: {
        bp_pat_t *def = _bp_definition(start, end);
        if (def) return def;
        // Reference
        if (!isalpha(c) && c != '_') return NULL;
        str = after_name(start, end);
        size_t namelen = (size_t)(str - start);
        return Pattern(BP_REF, start, str, 0, -1, .name=start, .len=namelen);
    }
    }
}

//
// Similar to bp_simplepattern, except that the pattern begins with an implicit
// '}' open quote that can be closed with '{'
//
public maybe_pat_t bp_stringpattern(const char *str, const char *end)
{
    __TRY_PATTERN__
    if (!end) end = str + strlen(str);
    char *start = (char*)str;
    while (str < end && *str != '{')
        str = next_char(str, end);
    size_t len = (size_t)(str - start);
    bp_pat_t *pat = len > 0 ? Pattern(BP_STRING, start, str, len, (ssize_t)len, .string=strndup(start, len)) : NULL;
    str += 1;
    if (str < end) {
        bp_pat_t *interp = bp_pattern_nl(str, end, true);
        if (interp)
            pat = chain_together(pat, interp);
        pat->end = end;
    }
    __END_TRY_PATTERN__
    return (maybe_pat_t){.success = true, .value.pat = pat};
}

//
// Wrapper for _bp_simplepattern() that expands any postfix operators (~, !~)
//
static bp_pat_t *bp_simplepattern(const char *str, const char *end)
{
    const char *start = str;
    bp_pat_t *pat = _bp_simplepattern(str, end, false);
    if (pat == NULL) return pat;
    str = pat->end;

    // Expand postfix operators (if any)
    while (str < end) {
        enum bp_pattype_e type;
        if (matchchar(&str, '~', false, end))
            type = BP_MATCH;
        else if (matchstr(&str, "!~", false, end))
            type = BP_NOT_MATCH;
        else break;

        bp_pat_t *first = pat;
        bp_pat_t *second = bp_simplepattern(str, end);
        if (!second)
            parse_err(str, str, "There should be a valid pattern here");

        pat = type == BP_MATCH ? 
            Pattern(BP_MATCH, start, second->end, first->min_matchlen, first->max_matchlen, .pat=first, .must_match=second)
            : Pattern(BP_NOT_MATCH, start, second->end, first->min_matchlen, first->max_matchlen, .pat=first, .must_not_match=second);
        str = pat->end;
    }

    return pat;
}

//
// Given a pattern and a replacement string, compile the two into a BP
// replace pattern.
//
public maybe_pat_t bp_replacement(bp_pat_t *replacepat, const char *replacement, const char *end)
{
    const char *p = replacement;
    if (!end) end = replacement + strlen(replacement);
    __TRY_PATTERN__
    for (; p < end; p++) {
        if (*p == '\\') {
            if (!p[1] || p[1] == '\n')
                parse_err(p, p, "There should be an escape sequence or pattern here after this backslash.");
            ++p;
        }
    }
    __END_TRY_PATTERN__
    size_t rlen = (size_t)(p-replacement);
    char *rcpy = new(char[rlen + 1]);
    memcpy(rcpy, replacement, rlen);
    bp_pat_t *pat = Pattern(BP_REPLACE, replacepat->start, replacepat->end, replacepat->min_matchlen, replacepat->max_matchlen,
                         .pat=replacepat, .text=rcpy, .len=rlen);
    return (maybe_pat_t){.success = true, .value.pat = pat};
}

static bp_pat_t *bp_pattern_nl(const char *str, const char *end, bool allow_nl)
{
    str = after_spaces(str, allow_nl, end);
    bp_pat_t *pat = bp_simplepattern(str, end);
    if (pat != NULL) pat = expand_choices(pat, end, allow_nl);
    if (matchstr(&str, "=>", allow_nl, end))
        pat = expand_replacements(pat ? pat : Pattern(BP_STRING, str-2, str-2, 0, 0), end, allow_nl);
    return pat;
}

//
// Return a new back reference to an existing match.
//
public bp_pat_t *bp_raw_literal(const char *str, size_t len)
{
    return Pattern(BP_STRING, str, &str[len], len, (ssize_t)len, .string=strndup(str, len));
}

//
// Compile a string representing a BP pattern into a pattern object.
//
public maybe_pat_t bp_pattern(const char *str, const char *end)
{
    str = after_spaces(str, true, end);
    if (!end) end = str + strlen(str);
    __TRY_PATTERN__
    bp_pat_t *ret = bp_pattern_nl(str, end, false);
    __END_TRY_PATTERN__
    if (ret && after_spaces(ret->end, true, end) < end)
        return (maybe_pat_t){.success = false, .value.error.start = ret->end, .value.error.end = end, .value.error.msg = "Failed to parse this part of the pattern"};
    else if (ret)
        return (maybe_pat_t){.success = true, .value.pat = ret};
    else
        return (maybe_pat_t){.success = false, .value.error.start = str, .value.error.end = end, .value.error.msg = "Failed to parse this pattern"};
}

public void free_all_pats(void)
{
    while (allocated_pats) {
        bp_pat_t *tofree = allocated_pats;
        allocated_pats = tofree->next;
        delete(&tofree);
    }
}

public void delete_pat(bp_pat_t **at_pat, bool recursive)
{
    bp_pat_t *pat = *at_pat;
    if (!pat) return;

#define T(tag, ...) case tag: { auto _data = When(pat, tag); __VA_ARGS__; break; }
#define F(field) delete_pat(&_data->field, true)
    if (recursive) {
        switch (pat->type) {
        T(BP_DEFINITIONS, F(meaning), F(next_def))
        T(BP_REPEAT, F(sep), F(repeat_pat))
        T(BP_CHAIN, F(first), F(second))
        T(BP_UPTO, F(target), F(skip))
        T(BP_UPTO_STRICT, F(target), F(skip))
        T(BP_OTHERWISE, F(first), F(second))
        T(BP_MATCH, F(pat), F(must_match))
        T(BP_NOT_MATCH, F(pat), F(must_not_match))
        T(BP_REPLACE, F(pat))
        T(BP_CAPTURE, F(pat))
        T(BP_TAGGED, F(pat))
        T(BP_NOT, F(pat))
        T(BP_AFTER, F(pat))
        T(BP_BEFORE, F(pat))
        T(BP_LEFTRECURSION, F(fallback))
        T(BP_STRING, if (_data->string) { free((char*)_data->string); _data->string = NULL; })
        default: break;
        }
    }
#undef F
#undef T

    if (pat->home) *(pat->home) = pat->next;
    if (pat->next) pat->next->home = pat->home;
    delete(at_pat);
}

int fprint_pattern(FILE *stream, bp_pat_t *pat)
{
    if (!pat) return fputs("(null)", stream);
    switch (pat->type) {
#define CASE(name, ...) case BP_ ## name: { __auto_type data = pat->__tagged.BP_##name; (void)data; int _printed = fputs(#name, stream); __VA_ARGS__; return _printed; }
#define FMT(...) _printed += fprintf(stream, __VA_ARGS__)
#define PAT(p) _printed += fprint_pattern(stream, p)
        CASE(ERROR)
        CASE(ANYCHAR)
        CASE(ID_START)
        CASE(ID_CONTINUE)
        CASE(STRING, FMT("(\"%s\")", data.string))
        CASE(RANGE, FMT("('%c'-'%c')", data.low, data.high))
        CASE(NOT, FMT("("); PAT(data.pat); FMT(")"))
        CASE(UPTO, FMT("("); PAT(data.target); FMT(", skip="); PAT(data.skip); FMT(")"))
        CASE(UPTO_STRICT, FMT("("); PAT(data.target); FMT(", skip=)"); PAT(data.skip))
        CASE(REPEAT, FMT("(%u-%d, ", data.min, data.max); PAT(data.repeat_pat); FMT(", sep="); PAT(data.sep); FMT(")"))
        CASE(BEFORE, FMT("("); PAT(data.pat); FMT(")"))
        CASE(AFTER, FMT("("); PAT(data.pat); FMT(")"))
        CASE(CAPTURE, FMT("("); PAT(data.pat); FMT(", name=%.*s, backref=%s)", data.namelen, data.name, data.backreffable ? "yes" : "no"))
        CASE(OTHERWISE, FMT("("); PAT(data.first); FMT(", "); PAT(data.second); FMT(")"))
        CASE(CHAIN, FMT("("); PAT(data.first); FMT(", "); PAT(data.second); FMT(")"))
        CASE(MATCH, FMT("("); PAT(data.pat); FMT(", matches="); PAT(data.must_match); FMT(")"))
        CASE(NOT_MATCH, FMT("("); PAT(data.pat); FMT(", must_not_match="); PAT(data.must_not_match); FMT(")"))
        CASE(REPLACE, FMT("("); PAT(data.pat); FMT(", \"%.*s\")", data.pat, data.len, data.text))
        CASE(REF, FMT("(%.*s)", data.len, data.name))
        CASE(NODENT)
        CASE(CURDENT)
        CASE(START_OF_FILE)
        CASE(START_OF_LINE)
        CASE(END_OF_FILE)
        CASE(END_OF_LINE)
        CASE(WORD_BOUNDARY)
        CASE(DEFINITIONS, FMT("(%.*s=", data.namelen, data.name); PAT(data.meaning); FMT("); "); PAT(data.next_def))
        CASE(TAGGED, FMT("(%.*s=", data.namelen, data.name); PAT(data.pat); FMT(" backref=%s)", data.backreffable ? "yes" : "no"))
#undef PAT
#undef FMT
#undef P
    default: return fputs("???", stream);
    }
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
