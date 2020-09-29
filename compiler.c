/*
 * compiler.c - Compile strings into BPEG virtual machine code.
 */

#include "compiler.h"
#include "utils.h"

static vm_op_t *expand_chain(file_t *f, vm_op_t *first);
static vm_op_t *expand_choices(file_t *f, vm_op_t *first);
static vm_op_t *chain_together(vm_op_t *first, vm_op_t *second);
static void set_range(vm_op_t *op, ssize_t min, ssize_t max, vm_op_t *pat, vm_op_t *sep);

/*
 * Helper function to initialize a range object.
 */
static void set_range(vm_op_t *op, ssize_t min, ssize_t max, vm_op_t *pat, vm_op_t *sep)
{
    op->op = VM_REPEAT;
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
}

/*
 * Take an opcode and expand it into a chain of patterns if it's
 * followed by any patterns (e.g. "`x `y"), otherwise return
 * the original input.
 */
static vm_op_t *expand_chain(file_t *f, vm_op_t *first)
{
    vm_op_t *second = bpeg_simplepattern(f, first->end);
    if (second == NULL) return first;
    second = expand_chain(f, second);
    check(second->end > first->end, "No forward progress in chain!");
    return chain_together(first, second);
}

/*
 * Take an opcode and expand it into a chain of choices if it's
 * followed by any "/"-separated patterns (e.g. "`x/`y"), otherwise
 * return the original input.
 */
static vm_op_t *expand_choices(file_t *f, vm_op_t *first)
{
    first = expand_chain(f, first);
    const char *str = first->end;
    if (!matchchar(&str, '/')) return first;
    vm_op_t *second = bpeg_simplepattern(f, str);
    check(second, "Expected pattern after '/'");
    second = expand_choices(f, second);
    vm_op_t *choice = calloc(sizeof(vm_op_t), 1);
    choice->op = VM_OTHERWISE;
    choice->start = first->start;
    if (first->len == second->len)
        choice->len = first->len;
    else choice->len = -1;
    choice->end = second->end;
    choice->args.multiple.first = first;
    choice->args.multiple.second = second;
    return choice;
}

static vm_op_t *chain_together(vm_op_t *first, vm_op_t *second)
{
    if (first == NULL) return second;
    if (second == NULL) return first;
    vm_op_t *chain = calloc(sizeof(vm_op_t), 1);
    chain->op = VM_CHAIN;
    chain->start = first->start;
    if (first->len >= 0 && second->len >= 0)
        chain->len = first->len + second->len;
    else chain->len = -1;
    chain->end = second->end;
    chain->args.multiple.first = first;
    chain->args.multiple.second = second;
    return chain;
}

/*
 * Compile a string of BPEG code into virtual machine opcodes
 */
vm_op_t *bpeg_simplepattern(file_t *f, const char *str)
{
    str = after_spaces(str);
    if (!*str) return NULL;
    vm_op_t *op = calloc(sizeof(vm_op_t), 1);
    op->start = str;
    op->len = -1;
    char c = *str;
    ++str;
    switch (c) {
        // Any char (dot) ($. is multiline anychar)
        case '.': {
            if (*str == '.') { // ".."
                ++str;
                if (*str == '.') { // "..."
                    ++str;
                    op->multiline = 1;
                }
                vm_op_t *till = bpeg_simplepattern(f, str);
                op->op = VM_UPTO_AND;
                op->len = -1;
                op->args.pat = till;
                if (till)
                    str = till->end;
                break;
            } else {
              anychar:
                op->op = VM_ANYCHAR;
                op->len = 1;
                break;
            }
        }
        // Char literals
        case '`': {
            char literal[2] = {*str, '\0'};
            ++str;
            check(literal[0], "Expected character after '`'\n");
            op->len = 1;
            if (matchchar(&str, '-')) { // Range
                char c2 = *str;
                check(c2, "Expected character after '-'");
                check(c2 >= literal[0], "Character range must be low-to-high");
                op->op = VM_RANGE;
                op->args.range.low = literal[0];
                op->args.range.high = c2;
                ++str;
            } else {
                op->op = VM_STRING;
                op->args.s = strdup(literal);
            }
            break;
        }
        // Escapes
        case '\\': {
            check(*str && *str != '\n', "Expected escape after '\\'");
            op->len = 1;
            char e = unescapechar(str, &str);
            if (*str == '-') { // Escape range (e.g. \x00-\xFF)
                ++str;
                char e2 = unescapechar(str, &str);
                check(e2, "Expected character after '-'");
                check(e2 >= e, "Character range must be low-to-high");
                op->op = VM_RANGE;
                op->args.range.low = e;
                op->args.range.high = e2;
            } else {
                char literal[2] = {e, '\0'};
                op->op = VM_STRING;
                op->args.s = strdup(literal);
            }
            break;
        }
        // String literal
        case '"': case '\'': case '\002': {
            char endquote = c == '\002' ? '\003' : c;
            char *literal = (char*)str;
            for (; *str && *str != endquote; str++) {
                if (*str == '\\') {
                    check(str[1], "Expected more string contents after backslash");
                    ++str;
                }
            }
            size_t len = (size_t)(str - literal);
            literal = strndup(literal, len);
            len = unescape_string(literal, literal, len);

            op->op = VM_STRING;
            op->len = (ssize_t)len;
            op->args.s = literal;

            check(matchchar(&str, endquote), "Missing closing quote");
            break;
        }
        // Not <pat>
        case '!': {
            vm_op_t *p = bpeg_simplepattern(f, str);
            check(p, "Expected pattern after '!'\n");
            str = p->end;
            op->op = VM_NOT;
            op->len = 0;
            op->args.pat = p;
            break;
        }
        // Number of repetitions: <N>(-<N> / - / + / "")
        case '0': case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9': {
            ssize_t min = -1, max = -1;
            --str;
            long n1 = strtol(str, (char**)&str, 10);
            if (matchchar(&str, '-')) {
                str = after_spaces(str);
                const char *start = str;
                long n2 = strtol(str, (char**)&str, 10);
                if (str == start) min = 0, max = n1;
                else min = n1, max = n2;
            } else if (matchchar(&str, '+')) {
                min = n1, max = -1;
            } else {
                min = n1, max = n1;
            }
            vm_op_t *pat = bpeg_simplepattern(f, str);
            check(pat, "Expected pattern after repetition count");
            str = pat->end;
            str = after_spaces(str);
            vm_op_t *sep = NULL;
            if (matchchar(&str, '%')) {
                sep = bpeg_simplepattern(f, str);
                check(sep, "Expected pattern for separator after '%%'");
                str = sep->end;
            } else {
                str = pat->end;
            }
            set_range(op, min, max, pat, sep);
            break;
        }
        // Lookbehind
        case '<': {
            vm_op_t *pat = bpeg_simplepattern(f, str);
            check(pat, "Expected pattern after <");
            str = pat->end;
            check(pat->len != -1, "Lookbehind patterns must have a fixed length");
            str = pat->end;
            op->op = VM_AFTER;
            op->len = 0;
            op->args.pat = pat;
            break;
        }
        // Lookahead
        case '>': {
            vm_op_t *pat = bpeg_simplepattern(f, str);
            check(pat, "Expected pattern after >");
            str = pat->end;
            op->op = VM_BEFORE;
            op->len = 0;
            op->args.pat = pat;
            break;
        }
        // Parentheses
        case '(': {
            free(op);
            op = bpeg_simplepattern(f, str);
            check(op, "Expected pattern inside parentheses");
            op = expand_choices(f, op);
            str = op->end;
            str = after_spaces(str);
            check(matchchar(&str, ')'), "Expected closing ')' instead of \"%s\"", str);
            break;
        }
        // Square brackets
        case '[': {
            vm_op_t *pat = bpeg_simplepattern(f, str);
            check(pat, "Expected pattern inside square brackets");
            pat = expand_choices(f, pat);
            str = pat->end;
            str = after_spaces(str);
            check(matchchar(&str, ']'), "Expected closing ']' instead of \"%s\"", str);
            set_range(op, 0, 1, pat, NULL);
            break;
        }
        // Capture
        case '@': {
            op->op = VM_CAPTURE;
            const char *a = *str == '!' ? &str[1] : after_name(str);
            if (a > str && after_spaces(a)[0] == '=' && after_spaces(a)[1] != '>') {
                op->args.capture.name = strndup(str, (size_t)(a-str));
                str = after_spaces(a) + 1;
            }
            vm_op_t *pat = bpeg_simplepattern(f, str);
            check(pat, "Expected pattern after @");
            str = pat->end;
            op->args.capture.capture_pat = pat;
            op->len = pat->len;
            break;
        }
        // Replacement
        case '{': {
            str = after_spaces(str);
            vm_op_t *pat = NULL;
            if (strncmp(str, "=>", 2) == 0) {
                str += strlen("=>");
            } else {
                pat = bpeg_simplepattern(f, str);
                check(pat, "Invalid pattern after '{'");
                pat = expand_choices(f, pat);
                str = pat->end;
                str = after_spaces(str);
                check(matchchar(&str, '=') && matchchar(&str, '>'),
                      "Expected '=>' after pattern in replacement");
            }
            str = after_spaces(str);

            char quote = *str;
            const char *replacement;
            if (matchchar(&str, '}')) {
                replacement = strdup("");
            } else {
                check(matchchar(&str, '"') || matchchar(&str, '\''),
                      "Expected string literal for replacement");
                replacement = str;
                for (; *str && *str != quote; str++) {
                    if (*str == '\\') {
                        check(str[1], "Expected more string contents after backslash");
                        ++str;
                    }
                }
                replacement = strndup(replacement, (size_t)(str-replacement));
                check(matchchar(&str, quote), "Expected closing quote");
                check(matchchar(&str, '}'), "Expected a closing '}'");
            }
            op->op = VM_REPLACE;
            op->args.replace.replace_pat = pat;
            op->args.replace.replacement = replacement;
            if (pat != NULL) op->len = pat->len;
            break;
        }
        // Special rules:
        case '_': case '^': case '$': {
            if (matchchar(&str, c)) { // double __, ^^, $$
                char tmp[3] = {c, c, '\0'};
                op->args.s = strdup(tmp);
            } else if (c == '$' && matchchar(&str, '.')) { // $. (multi-line anychar)
                op->multiline = 1;
                goto anychar;
            } else {
                op->args.s = strndup(&c, 1);
            }
            if (*after_spaces(str) == ':') {
                free((char*)op->args.s);
                free(op);
                return NULL;
            }
            op->op = VM_REF;
            break;
        }
        case '|': {
            op->op = VM_NODENT;
            break;
        }
        default: {
            // Reference
            if (isalpha(c)) {
                --str;
                const char *refname = str;
                str = after_name(str);
                if (*after_spaces(str) == ':') {
                    free(op);
                    return NULL;
                }
                op->op = VM_REF;
                op->args.s = strndup(refname, (size_t)(str - refname));
                break;
            } else {
                free(op);
                return NULL;
            }
        }
    }
    op->end = str;

    // Postfix operators:
  postfix:
    if (f ? str >= f->end : !*str) return op;
    str = after_spaces(str);
    if (*str == '*' || *str == '+' || *str == '?') { // Repetitions: <pat>*, <pat>+, <pat>?
        char operator = *str;
        ++str;
        vm_op_t *pat = op;
        vm_op_t *sep = NULL;
        if (operator != '?' && matchchar(&str, '%')) {
            sep = bpeg_simplepattern(f, str);
            check(sep, "Expected pattern for separator after '%%'");
            str = sep->end;
        }
        op = calloc(sizeof(vm_op_t), 1);
        set_range(op, operator == '+' ? 1 : 0, operator == '?' ? 1 : -1, pat, sep);
        op->start = pat->start;
        op->end = str;
        op->len = -1;
        goto postfix;
    } else if (strncmp(str, "==", 2) == 0) { // Equality <pat1>==<pat2>
        str = after_spaces(str+2);
        vm_op_t *first = op;
        vm_op_t *second = bpeg_simplepattern(f, str);
        check(second, "Expected pattern after '=='");
        check(first->len == -1 || second->len == -1 || first->len == second->len,
              "Two patterns cannot possibly match the same (different lengths: %ld != %ld)",
              first->len, second->len);
        op = calloc(sizeof(vm_op_t), 1);
        op->op = VM_EQUAL;
        op->start = str;
        op->end = second->end;
        op->len = (first->len == -1 || second->len == -1) ? -1 : first->len;
        op->args.multiple.first = first;
        op->args.multiple.second = second;
        str = op->end;
        goto postfix;
    }

    return op;
}

/*
 * Similar to bpeg_simplepattern, except that the pattern begins with an implicit, unclosable quote.
 */
vm_op_t *bpeg_stringpattern(file_t *f, const char *str)
{
    vm_op_t *ret = NULL;
    while (*str) {
        vm_op_t *strop = calloc(sizeof(vm_op_t), 1);
        strop->start = str;
        strop->len = 0;
        strop->op = VM_STRING;
        char *literal = (char*)str;
        vm_op_t *interp = NULL;
        for (; *str; str++) {
            if (*str == '\\') {
                check(str[1], "Expected more string contents after backslash");
                const char *after_escape;
                char e = unescapechar(&str[1], &after_escape);
                if (e != str[1]) {
                    str = after_escape - 1;
                    continue;
                }
                if (str[1] == '\\') {
                    ++str;
                    continue;
                }
                interp = bpeg_simplepattern(f, str + 1);
                check(interp != NULL, "No valid BPEG pattern detected after backslash");
                break;
            }
        }
        // End of string
        size_t len = (size_t)(str - literal);
        literal = strndup(literal, len);
        len = unescape_string(literal, literal, len);
        strop->len = (ssize_t)len;
        strop->args.s = literal;
        strop->end = str;

        if (strop->len == 0) {
            free(strop);
            strop = NULL;
        } else {
            ret = chain_together(ret, strop);
        }
        if (interp) {
            ret = chain_together(ret, interp);
            str = interp->end;
            // allow terminating seq
            matchchar(&str, ';');
        }
    }
    return ret;
}

/*
 * Given a pattern and a replacement string, compile the two into a replacement
 * VM opcode.
 */
vm_op_t *bpeg_replacement(vm_op_t *pat, const char *replacement)
{
    vm_op_t *op = calloc(sizeof(vm_op_t), 1);
    op->op = VM_REPLACE;
    op->start = pat->start;
    op->len = pat->len;
    op->args.replace.replace_pat = pat;
    const char *p = replacement;
    for (; *p; p++) {
        if (*p == '\\') {
            check(p[1], "Expected more string contents after backslash");
            ++p;
        }
    }
    replacement = strndup(replacement, (size_t)(p-replacement));
    op->args.replace.replacement = replacement;
    return op;
}

vm_op_t *bpeg_pattern(file_t *f, const char *str)
{
    vm_op_t *op = bpeg_simplepattern(f, str);
    if (op != NULL) op = expand_choices(f, op);
    return op;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
