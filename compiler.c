//
// compiler.c - Compile strings into BP virtual machine code.
//

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compiler.h"
#include "utils.h"

#define file_err(f, ...) do { fprint_line(stderr, f, __VA_ARGS__); exit(1); } while(0)

__attribute__((nonnull))
static vm_op_t *expand_chain(file_t *f, vm_op_t *first);
__attribute__((nonnull))
static vm_op_t *expand_choices(file_t *f, vm_op_t *first);
__attribute__((nonnull))
static vm_op_t *_bp_simplepattern(file_t *f, const char *str);
__attribute__((nonnull(1)))
static vm_op_t *chain_together(file_t *f,vm_op_t *first, vm_op_t *second);
__attribute__((nonnull(1,2,3,6)))
static vm_op_t *new_range(file_t *f, const char *start, const char *end, ssize_t min, ssize_t max, vm_op_t *pat, vm_op_t *sep);

//
// Allocate a new opcode for this file (ensuring it will be automatically freed
// when the file is freed)
//
vm_op_t *new_op(file_t *f, const char *start, enum VMOpcode type)
{
    allocated_op_t *tracker = new(allocated_op_t);
    tracker->next = f->ops;
    f->ops = tracker;
    tracker->op.type = type;
    tracker->op.start = start;
    tracker->op.len = -1;
    return &tracker->op;
}

//
// Helper function to initialize a range object.
//
static vm_op_t *new_range(file_t *f, const char *start, const char *end, ssize_t min, ssize_t max, vm_op_t *pat, vm_op_t *sep)
{
    vm_op_t *op = new_op(f, start, VM_REPEAT);
    if (pat->len >= 0 && (sep == NULL || sep->len >= 0) && min == max && min >= 0)
        op->len = pat->len * min + (sep == NULL || min == 0 ? 0 : sep->len * (min-1));
    else
        op->len = -1;
    op->args.repetitions.min = min;
    op->args.repetitions.max = max;
    op->args.repetitions.repeat_pat = pat;
    op->args.repetitions.sep = sep;
    if (!op->start) op->start = pat->start;
    if (!op->end) op->end = pat->end;
    if (sep) {
        if (sep->start < op->start) op->start = sep->start;
        if (sep->end > op->end) op->end = sep->end;
    }
    op->end = end;
    return op;
}

//
// Take an opcode and expand it into a chain of patterns if it's followed by
// any patterns (e.g. "`x `y"), otherwise return the original input.
//
static vm_op_t *expand_chain(file_t *f, vm_op_t *first)
{
    vm_op_t *second = bp_simplepattern(f, first->end);
    if (second == NULL) return first;
    second = expand_chain(f, second);
    if (second->end <= first->end)
        file_err(f, second->end, second->end,
                 "This chain is not parsing properly");
    return chain_together(f, first, second);
}

//
// Take an opcode and parse any "=>" replacements and then expand it into a
// chain of choices if it's followed by any "/"-separated patterns (e.g.
// "`x/`y"), otherwise return the original input.
//
static vm_op_t *expand_choices(file_t *f, vm_op_t *first)
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
        
        vm_op_t *pat = first;
        first = new_op(f, pat->start, VM_REPLACE);
        first->args.replace.pat = pat;
        first->args.replace.text = replacement;
        first->args.replace.len = replace_len;
        first->len = pat->len;
        first->end = str;
    }

    if (!matchchar(&str, '/')) return first;
    vm_op_t *second = bp_simplepattern(f, str);
    if (!second)
        file_err(f, str, str, "There should be a pattern here after a '/'");
    second = expand_choices(f, second);
    vm_op_t *choice = new_op(f, first->start, VM_OTHERWISE);
    if (first->len == second->len)
        choice->len = first->len;
    else choice->len = -1;
    choice->end = second->end;
    choice->args.multiple.first = first;
    choice->args.multiple.second = second;
    return choice;
}

//
// Given two patterns, return a new opcode for the first pattern followed by
// the second. If either pattern is NULL, return the other.
//
static vm_op_t *chain_together(file_t *f, vm_op_t *first, vm_op_t *second)
{
    if (first == NULL) return second;
    if (second == NULL) return first;
    check(first->type != VM_CHAIN, "A chain should not be the first item in a chain.");
    vm_op_t *chain = new_op(f, first->start, VM_CHAIN);
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
vm_op_t *bp_simplepattern(file_t *f, const char *str)
{
    vm_op_t *op = _bp_simplepattern(f, str);
    if (op == NULL) return op;

    check(op->end != NULL, "op->end is uninitialized!");

    // Expand postfix operators (if any)
    str = after_spaces(op->end);
    while (str+2 < f->end && (matchstr(&str, "!=") || matchstr(&str, "=="))) { // Equality <pat1>==<pat2> and inequality <pat1>!=<pat2>
        int equal = str[-2] == '=';
        vm_op_t *first = op;
        vm_op_t *second = bp_simplepattern(f, str);
        if (!second)
            file_err(f, str, str, "The '%c=' operator expects a pattern before and after.", equal?'=':'!');
        if (equal) {
            if (!(first->len == -1 || second->len == -1 || first->len == second->len))
                file_err(f, op->start, second->end,
                  "These two patterns cannot possibly give the same result (different lengths: %ld != %ld)",
                  first->len, second->len);
        }
        op = new_op(f, str, equal ? VM_EQUAL : VM_NOT_EQUAL);
        op->end = second->end;
        op->len = first->len != -1 ? first->len : second->len;
        op->args.multiple.first = first;
        op->args.multiple.second = second;
        str = op->end;
        str = after_spaces(str);
    }

    return op;
}

//
// Compile a string of BP code into virtual machine opcodes
//
static vm_op_t *_bp_simplepattern(file_t *f, const char *str)
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
                vm_op_t *op = new_op(f, start, VM_UPTO_AND);
                ++str;
                vm_op_t *till = bp_simplepattern(f, str);
                op->args.multiple.first = till;
                if (till)
                    str = till->end;
                if (matchchar(&str, '%')) {
                    vm_op_t *skip = bp_simplepattern(f, str);
                    if (!skip)
                        file_err(f, str, str, "There should be a pattern to skip here after the '%%'");
                    op->args.multiple.second = skip;
                    str = skip->end;
                }
                op->end = str;
                return op;
            } else {
                vm_op_t *op = new_op(f, start, VM_ANYCHAR);
                op->len = 1;
                op->end = str;
                return op;
            }
        }
        // Char literals
        case '`': {
            vm_op_t *all = NULL;
            do {
                char c = *str;
                if (!c || c == '\n')
                    file_err(f, str, str, "There should be a character here after the '`'");
                const char *opstart = str-1;

                vm_op_t *op;
                ++str;
                if (matchchar(&str, '-')) { // Range
                    char c2 = *str;
                    if (!c2 || c2 == '\n')
                        file_err(f, str, str, "There should be a character here to complete the character range.");
                    op = new_op(f, opstart, VM_RANGE);
                    if (c < c2) {
                        op->args.range.low = (unsigned char)c;
                        op->args.range.high = (unsigned char)c2;
                    } else {
                        op->args.range.low = (unsigned char)c2;
                        op->args.range.high = (unsigned char)c;
                    }
                    ++str;
                } else {
                    op = new_op(f, opstart, VM_STRING);
                    char *s = xcalloc(sizeof(char), 2);
                    s[0] = c;
                    op->args.s = s;
                }

                op->len = 1;
                op->end = str;

                if (all == NULL) {
                    all = op;
                } else {
                    vm_op_t *either = new_op(f, all->start, VM_OTHERWISE);
                    either->end = op->end;
                    either->args.multiple.first = all;
                    either->args.multiple.second = op;
                    either->len = 1;
                    all = either;
                }
                op = NULL;
            } while (matchchar(&str, ','));

            return all;
        }
        // Escapes
        case '\\': {
            if (!*str || *str == '\n')
                file_err(f, str, str, "There should be an escape sequence here after this backslash.");

            if (matchchar(&str, 'N')) { // \N (nodent)
                vm_op_t *op = new_op(f, start, VM_NODENT);
                op->end = str;
                return op;
            }

            vm_op_t *op;
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
                op = new_op(f, opstart, VM_RANGE);
                op->args.range.low = e;
                op->args.range.high = e2;
            } else {
                op = new_op(f, opstart, VM_STRING);
                char *s = xcalloc(sizeof(char), 2);
                s[0] = (char)e;
                op->args.s = s;
            }
            op->len = 1;
            op->end = str;
            return op;
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

            vm_op_t *op = new_op(f, start, VM_STRING);
            op->len = (ssize_t)len;
            op->args.s = literal;

            if (!matchchar(&str, endquote))
                file_err(f, start, str, "This string doesn't have a closing quote.");

            op->end = str;
            return op;
        }
        // Not <pat>
        case '!': {
            vm_op_t *p = bp_simplepattern(f, str);
            if (!p) file_err(f, str, str, "There should be a pattern after this '!'");
            vm_op_t *op = new_op(f, start, VM_NOT);
            op->len = 0;
            op->args.pat = p;
            op->end = p->end;
            return op;
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
            vm_op_t *pat = bp_simplepattern(f, str);
            if (!pat)
                file_err(f, str, str, "There should be a pattern after this repetition count.");
            str = pat->end;
            vm_op_t *sep = NULL;
            if (matchchar(&str, '%')) {
                sep = bp_simplepattern(f, str);
                if (!sep)
                    file_err(f, str, str, "There should be a separator pattern after this '%%'");
                str = sep->end;
            } else {
                str = pat->end;
            }
            return new_range(f, start, str, min, max, pat, sep);
        }
        // Lookbehind
        case '<': {
            vm_op_t *pat = bp_simplepattern(f, str);
            if (!pat)
                file_err(f, str, str, "There should be a pattern after this '<'");
            str = pat->end;
            if (pat->len == -1)
                file_err(f, start, pat->end,
                         "Sorry, variable-length lookbehind patterns like this are not supported.\n"
                         "Please use a fixed-length lookbehind pattern instead.");
            str = pat->end;
            vm_op_t *op = new_op(f, start, VM_AFTER);
            op->len = 0;
            op->args.pat = pat;
            op->end = str;
            return op;
        }
        // Lookahead
        case '>': {
            vm_op_t *pat = bp_simplepattern(f, str);
            if (!pat)
                file_err(f, str, str, "There should be a pattern after this '>'");
            str = pat->end;
            vm_op_t *op = new_op(f, start, VM_BEFORE);
            op->len = 0;
            op->args.pat = pat;
            op->end = str;
            return op;
        }
        // Parentheses
        case '(': case '{': {
            char closing = c == '(' ? ')' : '}';
            vm_op_t *op = bp_simplepattern(f, str);
            if (!op)
                file_err(f, str, str, "There should be a valid pattern after this parenthesis.");
            op = expand_choices(f, op);
            str = op->end;
            if (!matchchar(&str, closing))
                file_err(f, start, str, "This parenthesis group isn't properly closed.");
            op->start = start;
            op->end = str;
            return op;
        }
        // Square brackets
        case '[': {
            vm_op_t *pat = bp_simplepattern(f, str);
            if (!pat)
                file_err(f, str, str, "There should be a valid pattern after this square bracket.");
            pat = expand_choices(f, pat);
            str = pat->end;
            if (!matchchar(&str, ']'))
                file_err(f, start, str, "This square bracket group isn't properly closed.");
            return new_range(f, start, str, 0, 1, pat, NULL);
        }
        // Repeating
        case '*': case '+': {
            ssize_t min = c == '*' ? 0 : 1;
            vm_op_t *pat = bp_simplepattern(f, str);
            if (!pat)
                file_err(f, str, str, "There should be a valid pattern here after the '%c'", c);
            str = pat->end;
            vm_op_t *sep = NULL;
            if (matchchar(&str, '%')) {
                sep = bp_simplepattern(f, str);
                if (!sep)
                    file_err(f, str, str, "There should be a separator pattern after the '%%' here.");
                str = sep->end;
            }
            return new_range(f, start, str, min, -1, pat, sep);
        }
        // Capture
        case '@': {
            vm_op_t *op = new_op(f, start, VM_CAPTURE);
            const char *a = *str == '!' ? &str[1] : after_name(str);
            if (a > str && after_spaces(a)[0] == '=' && after_spaces(a)[1] != '>') {
                op->args.capture.name = strndup(str, (size_t)(a-str));
                str = after_spaces(a) + 1;
            }
            vm_op_t *pat = bp_simplepattern(f, str);
            if (!pat)
                file_err(f, str, str, "There should be a valid pattern here to capture after the '@'");
            op->args.capture.capture_pat = pat;
            op->len = pat->len;
            op->end = pat->end;
            return op;
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
            vm_op_t *op = new_op(f, start, VM_REF);
            op->args.s = name;
            op->end = str;
            return op;
        }
        default: {
            // Reference
            if (!isalpha(c)) return NULL;
            --str;
            const char *refname = str;
            str = after_name(str);
            if (matchchar(&str, ':')) // Don't match definitions
                return NULL;
            vm_op_t *op = new_op(f, start, VM_REF);
            op->args.s = strndup(refname, (size_t)(str - refname));
            op->end = str;
            return op;
        }
    }
    return NULL;
}

//
// Similar to bp_simplepattern, except that the pattern begins with an implicit, unclosable quote.
//
vm_op_t *bp_stringpattern(file_t *f, const char *str)
{
    vm_op_t *ret = NULL;
    while (*str) {
        char *start = (char*)str;
        vm_op_t *interp = NULL;
        for (; *str; str++) {
            if (*str == '\\') {
                if (!str[1] || str[1] == '\n')
                    file_err(f, str, str, "There should be an escape sequence or pattern here after this backslash.");
                
                if (matchchar(&str, 'N')) { // \N (nodent)
                    interp = new_op(f, str-2, VM_NODENT);
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
            vm_op_t *strop = new_op(f, str, VM_STRING);
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
// Given a pattern and a replacement string, compile the two into a replacement
// VM opcode.
//
vm_op_t *bp_replacement(file_t *f, vm_op_t *pat, const char *replacement)
{
    vm_op_t *op = new_op(f, pat->start, VM_REPLACE);
    op->end = pat->end;
    op->len = pat->len;
    op->args.replace.pat = pat;
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
    op->args.replace.text = rcpy;
    op->args.replace.len = rlen;
    return op;
}

//
// Compile a string representing a BP pattern into an opcode object.
//
vm_op_t *bp_pattern(file_t *f, const char *str)
{
    vm_op_t *op = bp_simplepattern(f, str);
    if (op != NULL) op = expand_choices(f, op);
    return op;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
