/*
 * bpeg.h - Source code for the bpeg parser
 *
 * Grammar:
 *     # <comment>                 comment
 *     ` <c>                       character <c>
 *     ! <pat>                     no <pat>
 *     ^ <pat>                     upto <pat>
 *     & <pat>                     upto and including <pat>
 *     <N=1> + <pat> [% <sep="">]  <N> or more <pat>s (separated by <sep>)
 *     * <pat> [% <sep="">]        sugar for "0+ <pat> [% <sep>]"
 *     <N=1> - <pat> [% <sep="">]  <N> or fewer <pat>s (separated by <sep>)
 *     ? <pat>                     sugar for "1- <pat>"
 *     <N> - <M> <pat>             <N> to <M> (inclusive) <pat>s
 *     < <pat>                     after <pat>, ...
 *     > <pat>                     before <pat>, ...
 *     .                           any character
 *     <pat> / <alt>               <pat> otherwise <alt>
 *     ( <pat> )                   <pat>
 *     @ <pat>                     capture <pat>
 *     @ [ <name> ] <pat>          <pat> named <name>
 *     ; <name> = <pat>            <name> is defined to be <pat>
 *     { <pat> ~ <str> }           <pat> replaced with <str>
 *     "@1" or "@{1}"              first capture
 *     "@foo" or "@{foo}"          capture named "foo"
 */

#include "bpeg.h"

/*
 * Recursively deallocate a match object and return NULL
 */
static match_t *free_match(match_t *m)
{
    if (m->child) m->child = free_match(m->child);
    if (m->nextsibling) m->nextsibling = free_match(m->nextsibling);
    free(m);
    return NULL;
}

/*
 * Run virtual machine operation against a string and return
 * a match struct, or NULL if no match is found.
 * The returned value should be free()'d to avoid memory leaking.
 */
static match_t *match(const char *str, vm_op_t *op)
{
  tailcall:
    switch (op->op) {
        case VM_EMPTY: {
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = str;
            return m;
        }
        case VM_ANYCHAR: {
            if (!*str) return NULL;
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = str+1;
            return m;
        }
        case VM_STRING: {
            if (strncmp(str, op->args.s, op->len) != 0)
                return NULL;
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = str + op->len;
            return m;
        }
        case VM_RANGE: {
            if (*str < op->args.range.low || *str > op->args.range.high)
                return NULL;
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = str + 1;
            return m;
        }
        case VM_NOT: {
            match_t *m = match(str, op->args.pat);
            if (m != NULL) {
                m = free_match(m);
                return NULL;
            }
            m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = str;
            return m;
        }
        case VM_UPTO: case VM_UPTO_AND: {
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            while (*str) {
                match_t *p = match(str, op->args.pat);
                if (p == NULL) {
                    ++str;
                } else if (op->op == VM_UPTO) {
                    p = free_match(p);
                    m->end = str;
                    return m;
                } else {
                    m->end = p->end;
                    m->child = p;
                    return m;
                }
            }
            m = free_match(m);
            return NULL;
        }
        case VM_REPEAT: {
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = str;
            if (op->args.repetitions.max == 0) return m;

            match_t **dest = &m->child;

            size_t reps = 0;
            *dest = match(str, op->args.repetitions.repeat_pat);
            if (*dest != NULL) {
                ++reps;
                str = (*dest)->end;
                dest = &(*dest)->nextsibling;

                if (op->args.repetitions.sep != NULL) {
                    for (; reps < (size_t)op->args.repetitions.max; reps++) {
                        match_t *sep = match(str, op->args.repetitions.sep);
                        if (sep == NULL) break;
                        str = sep->end;
                        match_t *p = match(str, op->args.repetitions.repeat_pat);
                        if (p == NULL) {
                            p = free_match(p);
                            break;
                        }
                        str = p->end;
                        *dest = sep;
                        sep->nextsibling = p;
                        dest = &p->nextsibling;
                    }
                } else {
                    for (; reps < (size_t)op->args.repetitions.max; reps++) {
                        *dest = match(str, op->args.repetitions.repeat_pat);
                        if (*dest == NULL) break;
                        str = (*dest)->end;
                        dest = &(*dest)->nextsibling;
                    }
                }
            }
            if ((ssize_t)reps < op->args.repetitions.min) {
                m = free_match(m);
                return NULL;
            }
            m->end = str;
            return m;
        }
        case VM_AFTER: {
            check(op->len != -1, "'<' is only allowed for fixed-length operations");
            // Check for necessary space:
            for (int i = 0; i < op->len; i++) {
                if (str[-i] == '\0') return NULL;
            }
            match_t *before = match(str-op->len, op->args.pat);
            if (before == NULL) return NULL;
            before = free_match(before);
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = str;
            return m;
        }
        case VM_BEFORE: {
            match_t *after = match(str, op->args.pat);
            if (after == NULL) return NULL;
            after = free_match(after);
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = str;
            return m;
        }
        case VM_CAPTURE: {
            match_t *p = match(str, op->args.pat);
            if (p == NULL) return NULL;
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = p->end;
            m->child = p;
            m->is_capture = 1;
            if (op->args.capture.name)
                m->name_or_replacement = op->args.capture.name;
            return m;
        }
        case VM_OTHERWISE: {
            match_t *m = match(str, op->args.multiple.first);
            if (m == NULL) m = match(str, op->args.multiple.second);
            return m;
        }
        case VM_CHAIN: {
            match_t *m1 = match(str, op->args.multiple.first);
            if (m1 == NULL) return NULL;
            match_t *m2 = match(m1->end, op->args.multiple.second);
            if (m2 == NULL) {
                m1 = free_match(m1);
                return NULL;
            }
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = m2->end;
            m->child = m1;
            m1->nextsibling = m2;
            return m;
        }
        case VM_REPLACE: {
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            if (op->args.replace.replace_pat) {
                match_t *p = match(str, op->args.replace.replace_pat);
                if (p == NULL) return NULL;
                m->child = p;
                m->end = p->end;
            } else {
                m->end = m->start;
            }
            m->is_replacement = 1;
            m->name_or_replacement = op->args.replace.replacement;
            return m;
        }
        case VM_REF: {
            for (size_t i = 0; i < ndefs; i++) {
                if (strcmp(defs[i].name, op->args.s) == 0) {
                    // Bingo!
                    op = defs[i].op;
                    goto tailcall;
                }
            }
            check(0, "Unknown identifier: '%s'", op->args.s);
            return NULL;
        }
        default: {
            fprintf(stderr, "Unknown opcode: %d", op->op);
            _exit(1);
            return NULL;
        }
    }
}

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
}

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

/*
 * Take an opcode and expand it into a chain of patterns if it's
 * followed by any patterns (e.g. "`x `y"), otherwise return
 * the original input.
 */
static vm_op_t *expand_chain(vm_op_t *first)
{
    vm_op_t *second = compile_bpeg(first->end);
    if (second == NULL) return first;
    check(second->end > first->end, "No forward progress in chain!");
    second = expand_chain(second);
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
 * Take an opcode and expand it into a chain of choices if it's
 * followed by any "/"-separated patterns (e.g. "`x/`y"), otherwise
 * return the original input.
 */
static vm_op_t *expand_choices(vm_op_t *first)
{
    first = expand_chain(first);
    const char *str = after_spaces(first->end);
    if (*str != '/') return first;
    ++str;
    vm_op_t *second = compile_bpeg(str);
    check(second, "Expected pattern after '/'");
    second = expand_chain(second);
    vm_op_t *choice = calloc(sizeof(vm_op_t), 1);
    choice->op = VM_OTHERWISE;
    choice->start = first->start;
    if (first->len == second->len)
        choice->len = first->len;
    else choice->len = -1;
    choice->end = second->end;
    choice->args.multiple.first = first;
    choice->args.multiple.second = second;
    return expand_choices(choice);
}

static vm_op_t *compile_bpeg(const char *str)
{
    if (!*str) return NULL;
    debug("Parsing \"%s\"...\n", str);
    str = after_spaces(str);
    check(*str, "Expected a pattern");
    vm_op_t *op = calloc(sizeof(vm_op_t), 1);
    op->start = str;
    op->len = -1;
    switch (*str) {
        // Any char (dot)
        case '.': {
            ++str;
            debug("Dot\n");
            op->op = VM_ANYCHAR;
            op->len = 1;
            break;
        }
        // Char literals
        case '`': {
            ++str;
            char c[2] = {*str, '\0'};
            ++str;
            check(c[0], "Expected character after '`'\n");
            op->len = 1;
            if (*str == ',') { // Range
                debug("Char range\n");
                char c2 = *(++str);
                check(c2, "Expected character after ','");
                op->op = VM_RANGE;
                op->args.range.low = c[0];
                op->args.range.high = c2;
            } else {
                debug("Char literal\n");
                op->op = VM_STRING;
                op->args.s = strdup(c);
            }
            break;
        }
        // Escapes
        case '\\': {
            ++str;
            debug("Escape sequence\n");
            check(*str, "Expected escape after '\\'");
            op->op = VM_STRING;
            op->len = 1;
            char c[2] = {*str, '\0'};
            switch (c[0]) {
                case 'a': c[0] = '\a'; break;
                case 'b': c[0] = '\b'; break;
                case 'n': c[0] = '\n'; break;
                case 'r': c[0] = '\r'; break;
                case 't': c[0] = '\t'; break;
                case 'v': c[0] = '\v'; break;
                case 'x': { // Hex
                    static const char hextable[255] = {
                        ['0']=0x10, ['1']=0x1, ['2']=0x2, ['3']=0x3, ['4']=0x4,
                        ['5']=0x5, ['6']=0x6, ['7']=0x7, ['8']=0x8, ['9']=0x9,
                        ['a']=0xa, ['b']=0xb, ['c']=0xc, ['d']=0xd, ['e']=0xe, ['f']=0xf,
                        ['A']=0xa, ['B']=0xb, ['C']=0xc, ['D']=0xd, ['E']=0xe, ['F']=0xf,
                    };
                    if (hextable[(int)str[1]] && hextable[(int)str[2]])
                        c[0] = (hextable[(int)str[1]] << 4) | (hextable[(int)str[2]] & 0xF);
                    break;
                }
                case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': { // Octal
                    c[0] = c[0] - '0';
                    ++str;
                    if ('0' <= *str && *str <= '7') {
                        c[0] = (c[0] << 3) | (*str - '0');
                        ++str;
                    }
                    if ('0' <= *str && *str <= '7') {
                        c[0] = (c[0] << 3) | (*str - '0');
                        ++str;
                    }
                    break;
                }
                default: {
                    check(0, "Invalid escape sequence");
                }
            }
            op->args.s = strdup(c);
            break;
        }
        // String literal
        case '"': case '\'': {
            char quote = *str;
            ++str;
            const char *literal = str;
            for (; *str && *str != quote; str++) {
                if (*str == '\\') {
                    // TODO: handle escape chars like \n
                    check(str[1], "Expected more string contents after backslash");
                    ++str;
                }
            }
            op->op = VM_STRING;
            op->len = (ssize_t)(str - literal);
            op->args.s = strndup(literal, (size_t)op->len);
            debug("String literal: %c%s%c\n", quote, op->args.s, quote);
            check(*str == quote, "Missing closing quote");
            ++str;
            break;
        }
        // Not <pat>
        case '!': {
            ++str;
            debug("Not pattern\n");
            vm_op_t *p = compile_bpeg(str);
            check(p, "Expected pattern after '!'\n");
            str = p->end;
            op->op = VM_NOT;
            op->len = 0;
            op->args.pat = p;
            break;
        }
        // Upto <pat>
        case '^': {
            ++str;
            debug("Upto pattern\n");
            vm_op_t *p = compile_bpeg(str);
            check(p, "Expected pattern after '^'\n");
            str = p->end;
            op->op = VM_UPTO;
            op->len = -1;
            op->args.pat = p;
            break;
        }
        // Upto and including <pat>
        case '&': {
            ++str;
            debug("Upto-and pattern\n");
            vm_op_t *p = compile_bpeg(str);
            check(p, "Expected pattern after '&'\n");
            str = p->end;
            op->op = VM_UPTO_AND;
            op->len = -1;
            op->args.pat = p;
            break;
        }
        // Number of repetitions: <N>(-<N> / - / + / "")
        case '0': case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9': {
            debug("Repetitions\n");
            ssize_t min = -1, max = -1;
            long n1 = strtol(str, (char**)&str, 10);
            str = after_spaces(str);
            switch (*str) {
                case '-': {
                    ++str;
                    str = after_spaces(str);
                    const char *start = str;
                    long n2 = strtol(str, (char**)&str, 10);
                    if (str == start) min = 0, max = n1;
                    else min = n1, max = n2;
                    break;
                }
                case '+': {
                    ++str;
                    min = n1, max = -1;
                    break;
                }
                default: {
                    min = n1, max = n1;
                    break;
                }
            }
            vm_op_t *pat = compile_bpeg(str);
            check(pat, "Expected pattern after repetition count");
            str = pat->end;
            str = after_spaces(str);
            if (*str == '%') {
                ++str;
                vm_op_t *sep = compile_bpeg(str);
                check(sep, "Expected pattern for separator after '%%'");
                str = sep->end;
                set_range(op, min, max, pat, sep);
            } else {
                set_range(op, min, max, pat, NULL);
            }
            debug("min = %lld max = %lld\n", (long long)op->args.repetitions.min, (long long)op->args.repetitions.max);
            break;
        }
        // Special repetitions:
        case '+': case '*': case '?': {
            debug("Special repetitions\n");
            ssize_t min = -1, max = -1;
            switch (*str) {
                case '+': min = 1, max = -1; break;
                case '*': min = 0, max = -1; break;
                case '?': min = 0, max = 1; break;
            }
            ++str;
            vm_op_t *pat = compile_bpeg(str);
            check(pat, "Expected pattern after +");
            str = pat->end;
            str = after_spaces(str);
            if (*str == '%') {
                ++str;
                vm_op_t *sep = compile_bpeg(str);
                check(sep, "Expected pattern for separator after '%%'");
                str = sep->end;
                set_range(op, min, max, pat, sep);
            } else {
                set_range(op, min, max, pat, NULL);
            }
            debug("min = %lld max = %lld\n", (long long)op->args.repetitions.min, (long long)op->args.repetitions.max);
            break;
        }
        // Lookbehind
        case '<': {
            ++str;
            debug("Lookbehind\n");
            vm_op_t *pat = compile_bpeg(str);
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
            ++str;
            debug("Lookahead\n");
            vm_op_t *pat = compile_bpeg(str);
            check(pat, "Expected pattern after >");
            str = pat->end;
            op->op = VM_BEFORE;
            op->len = 0;
            op->args.pat = pat;
            break;
        }
        // Parentheses
        case '(': {
            debug("Open paren (\n");
            ++str;
            free(op);
            op = compile_bpeg(str);
            check(op, "Expected pattern inside parentheses");
            op = expand_choices(op);
            str = op->end;
            str = after_spaces(str);
            check(*str == ')', "Expected closing parenthesis");
            ++str;
            debug("Close paren (\n");
            break;
        }
        // Capture
        case '@': {
            debug("Capture\n");
            ++str;
            op->op = VM_CAPTURE;
            str = after_spaces(str);
            if (*str == '[') {
                ++str;
                char *closing = strchr(str, ']');
                check(closing, "Expected closing ']'");
                op->args.capture.name = strndup(str, (size_t)(closing-str));
                debug("named \"%s\"\n", op->args.capture.name);
                str = closing;
                ++str;
            }
            vm_op_t *pat = compile_bpeg(str);
            check(pat, "Expected pattern after @");
            str = pat->end;
            op->args.capture.capture_pat = pat;
            op->len = pat->len;
            break;
        }
        // Replacement
        case '{': {
            debug("Replacement {\n");
            ++str;
            str = after_spaces(str);
            vm_op_t *pat = NULL;
            if (*str != '~') {
                pat = compile_bpeg(str);
                check(pat, "Expected pattern after '{'");
                pat = expand_choices(pat);
                str = pat->end;
                str = after_spaces(str);
                check(*str == '~', "Expected '~' after pattern in replacement");
            }
            ++str;
            str = after_spaces(str);

            char quote = *str;
            ++str;
            check(quote == '\'' || quote == '"',
                  "Expected string literal for replacement");
            const char *replacement = str;
            for (; *str && *str != quote; str++) {
                if (*str == '\\') {
                    check(str[1], "Expected more string contents after backslash");
                    ++str;
                }
            }
            replacement = strndup(replacement, (size_t)(str-replacement));
            ++str;
            str = after_spaces(str);
            check(*str == '}', "Expected a closing '}'");
            ++str;
            op->op = VM_REPLACE;
            op->args.replace.replace_pat = pat;
            op->args.replace.replacement = replacement;
            debug(" rep = \"%s\"\n", replacement);
            debug("}\n");
            if (pat != NULL) op->len = pat->len;
            break;
        }
        // Whitespace
        case '_': {
            debug("Whitespace\n");
            ++str;
            op->op = VM_REF;
            op->args.s = strdup("_");
            break;
        }
        default: {
            // Reference
            if (isalpha(*str)) {
                const char *refname = str;
                size_t len = 1;
                for (++str; isalnum(*str); ++str)
                    ++len;
                op->op = VM_REF;
                debug("Ref: %s\n", refname);
                op->args.s = strndup(refname, len);
                break;
            } else {
                free(op);
                return NULL;
            }
        }
    }
    op->end = str;
    return op;
}

static vm_op_t *load_def(const char *name, const char *def)
{
    defs[ndefs].name = name;
    vm_op_t *op = compile_bpeg(def);
    defs[ndefs].op = op;
    ++ndefs;
    return op;
}

static void load_defs(void)
{
    load_def("_", "*(` /\\t/\\n/\\r)");
    load_def("__", "+(` /\\t/\\n/\\r)");
    load_def("nl", "\\n");
    load_def("crlf", "\\r\\n");
    load_def("abc", "`a,z");
    load_def("ABC", "`A,Z");
    load_def("Abc", "`a,z/`A,Z");
    load_def("digit", "`0,9");
    load_def("number", "+`0,9 ?(`. *`0,9) / `. +`0,9");
    load_def("hex", "`0,9/`a,f");
    load_def("Hex", "`0,9/`a,f/`A,F");
    load_def("HEX", "`0,9/`A,F");
    load_def("id", "(`a,z/`A,Z/`_) *(`a,z/`A,Z/`_/`0,9)");
    load_def("line", "&(?\\r\\n / !.)");
    load_def("parens", "`( *(parens / .) `)");
    load_def("braces", "`{ *(parens / .) `}");
    load_def("brackets", "`[ *(parens / .) `]");
    load_def("anglebraces", "`< *(parens / .) `>");
}

static match_t *get_capture_n(match_t *m, int *n)
{
    if (!m) return NULL;
    if (*n == 0) return m;
    if (m->is_capture && *n == 1) return m;
    if (m->is_capture) --(*n);
    for (match_t *c = m->child; c; c = c->nextsibling) {
        match_t *cap = get_capture_n(c, n);
        if (cap) return cap;
    }
    return NULL;
}

static match_t *get_capture_named(match_t *m, const char *name)
{
    if (m->is_capture && m->name_or_replacement && strcmp(m->name_or_replacement, name) == 0)
        return m;
    for (match_t *c = m->child; c; c = c->nextsibling) {
        match_t *cap = get_capture_named(c, name);
        if (cap) return cap;
    }
    return NULL;
}

static void print_match(match_t *m, const char *color)
{
    if (m->is_replacement) {
        printf("\033[0;7;34m");
        for (const char *r = m->name_or_replacement; *r; r++) {
            if (*r == '@') {
                ++r;
                match_t *cap = NULL;
                if (isdigit(*r)) {
                    int n = (int)strtol(r, (char**)&r, 10);
                    cap = get_capture_n(m->child, &n);
                    --r;
                } else if (*r == '[') {
                    char *closing = strchr(r+1, ']');
                    if (!closing) {
                        fputc('@', stdout);
                        --r;
                    } else {
                        char *name = strndup(r, (size_t)(closing-r));
                        cap = get_capture_named(m, name);
                        free(name);
                        r = closing;
                    }
                } else if (*r == '@') {
                    fputc('@', stdout);
                } else {
                    fputc('@', stdout);
                }
                if (cap != NULL) {
                    print_match(cap, "\033[0;7;35m");
                    printf("\033[0;7;34m");
                }
            } else {
                fputc(*r, stdout);
            }
        }
    } else {
        const char *prev = m->start;
        for (match_t *child = m->child; child; child = child->nextsibling) {
            if (child->start > prev)
                printf("%s%.*s", color, (int)(child->start - prev), prev);
            print_match(child, color);
            prev = child->end;
        }
        if (m->end > prev)
            printf("%s%.*s", color, (int)(m->end - prev), prev);
    }
}

int main(int argc, char *argv[])
{
    check(argc == 3, "Usage: bpeg <pat> <str>");
    load_defs();

    const char *lang = argc > 1 ? argv[1] : "'x''y'";
    vm_op_t *op = compile_bpeg(lang);
    check(op, "Failed to compile_bpeg input");
    op = expand_choices(op);
    
    const char *defs = after_spaces(op->end);
    while (*defs == ';') {
        defs = after_spaces(++defs);
        const char *name = defs;
        check(isalpha(*name), "Definition must begin with a name");
        while (isalpha(*defs)) ++defs;
        name = strndup(name, (size_t)(defs-name));
        defs = after_spaces(defs);
        check(*defs == '=', "Expected '=' in definition");
        ++defs;
        vm_op_t *def = load_def(name, defs);
        check(def, "Couldn't load definition");
        defs = def->end;
    }

    char *str = argc > 2 ? argv[2] : "xyz";

    // Ensure string has a null byte to the left:
    char *lpadded = calloc(sizeof(char), strlen(str)+2);
    stpcpy(&lpadded[1], str);
    str = &lpadded[1];

    match_t *m = match(str, op);
    if (m == NULL) {
        printf("No match\n");
    } else {
        print_match(m, "\033[0;7m");
        printf("\033[0;7;31m%s\n", m->end);
    }

    return 0;
}

//vim: ts=4
