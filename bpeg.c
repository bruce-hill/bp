// # <comment>                 comment
// ` <c>                       character <c>
// ! <pat>                     no <pat>
// ^ <pat>                     upto <pat>
// & <pat>                     upto and including <pat>
// <N=1> + <pat> [% <sep="">]  <N> or more <pat>s (separated by <sep>)
// * <pat> [% <sep="">]        sugar for "0+ <pat> [% <sep>]"
// <N=1> - <pat> [% <sep="">]  <N> or fewer <pat>s (separated by <sep>)
// ? <pat>                     sugar for "1- <pat>"
// <N> - <M> <pat>             <N> to <M> (inclusive) <pat>s
// < <pat>                     after <pat>, ...
// > <pat>                     before <pat>, ...
// .                           any character
// <pat> / <alt>               <pat> otherwise <alt>
// ( <pat> )                   <pat>
// @ <pat>                     capture <pat>
// @ [ <name> ] <pat>          <pat> named <name>
// ; <name> = <pat>            <name> is defined to be <pat>
// { <pat> ~ <str> }           <pat> replaced with <str>
// "@1" or "@{1}"              first capture
// "@foo" or "@{foo}"          capture named "foo"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define check(cond, ...) do { if (!(cond)) { fprintf(stderr, __VA_ARGS__); _exit(1); } } while(0)

typedef struct match_s {
    const char *start, *end;
    union {
        unsigned int is_capture:1;
        const char *name;
    } capture;
    const char *replacement;
    struct match_s *child, *nextsibling;
} match_t;

enum VM_OPTYPE {
    VM_EMPTY = 0,
    VM_ANYCHAR = 1,
    VM_STRING,
    VM_RANGE,
    VM_NOT,
    VM_UPTO,
    VM_UPTO_AND,
    VM_REPEAT,
    VM_BEFORE,
    VM_AFTER,
    VM_CAPTURE,
    VM_OTHERWISE,
    VM_CHAIN,
    VM_REPLACE,
    VM_REF,
};

typedef struct vm_op_s {
    enum VM_OPTYPE op;
    const char *start, *end;
    ssize_t len;
    union {
        const char *s;
        struct {
            char low, high;
        } range;
        struct {
            ssize_t min, max;
            struct vm_op_s *sep, *repeat_pat;
        } repetitions;
        struct {
            struct vm_op_s *first, *second;
        } multiple;
        struct {
            struct vm_op_s *replace_pat;
            const char *replacement;
        } replace;
        struct {
            struct vm_op_s *capture_pat;
            char *name;
        } capture;
        struct vm_op_s *pat;
    } args;
} vm_op_t;

static match_t *free_match(match_t *m);
static match_t *match(const char *str, vm_op_t *op);
static void set_range(vm_op_t *op, ssize_t min, ssize_t max, vm_op_t *pat, vm_op_t *sep);
static inline const char *skip_spaces(const char *str);
static vm_op_t *expand_choices(vm_op_t *op);
static vm_op_t *expand_chain(vm_op_t *first);
static vm_op_t *parse(const char *str);


typedef struct {
    const char *name;
    vm_op_t *op;
} def_t;

static def_t defs[1024] = {{NULL, NULL}};
size_t ndefs = 0;
static int verbose = 1;

#define debug(...) do { if (verbose) fprintf(stderr, __VA_ARGS__); } while(0)


static match_t *free_match(match_t *m)
{
    if (m->child) m->child = free_match(m->child);
    if (m->nextsibling) m->nextsibling = free_match(m->nextsibling);
    free(m);
    return NULL;
}

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
            for (; *str; ++str) {
                match_t *p = match(str, op->args.pat);
                if (p != NULL) {
                    if (op->op == VM_UPTO) {
                        p = free_match(p);
                        m->end = str;
                        return m;
                    } else {
                        m->end = p->end;
                        m->child = p;
                        return m;
                    }
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
            if (op->args.capture.name)
                m->capture.name = op->args.capture.name;
            else
                m->capture.is_capture = 1;
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
                m->end = p->end;
            } else {
                m->end = m->start;
            }
            // TODO: handle captures
            m->replacement = op->args.replace.replacement;
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

static inline const char *skip_spaces(const char *str)
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

static vm_op_t *expand_chain(vm_op_t *first)
{
    vm_op_t *second = parse(first->end);
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

static vm_op_t *expand_choices(vm_op_t *first)
{
    first = expand_chain(first);
    const char *str = skip_spaces(first->end);
    if (*str != '/') return first;
    ++str;
    vm_op_t *second = parse(str);
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

static vm_op_t *parse(const char *str)
{
    if (!*str) return NULL;
    debug("Parsing \"%s\"...\n", str);
    str = skip_spaces(str);
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
            vm_op_t *p = parse(str);
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
            vm_op_t *p = parse(str);
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
            vm_op_t *p = parse(str);
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
            str = skip_spaces(str);
            switch (*str) {
                case '-': {
                    ++str;
                    str = skip_spaces(str);
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
            vm_op_t *pat = parse(str);
            check(pat, "Expected pattern after repetition count");
            str = pat->end;
            str = skip_spaces(str);
            if (*str == '%') {
                ++str;
                vm_op_t *sep = parse(str);
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
            vm_op_t *pat = parse(str);
            check(pat, "Expected pattern after +");
            str = pat->end;
            str = skip_spaces(str);
            if (*str == '%') {
                ++str;
                vm_op_t *sep = parse(str);
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
            vm_op_t *pat = parse(str);
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
            vm_op_t *pat = parse(str);
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
            op = parse(str);
            check(op, "Expected pattern inside parentheses");
            op = expand_choices(op);
            str = op->end;
            str = skip_spaces(str);
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
            str = skip_spaces(str);
            if (*str == '[') {
                ++str;
                char *closing = strchr(str, ']');
                check(closing, "Expected closing ']'");
                op->args.capture.name = strndup(str, (size_t)(closing-str));
                debug("named \"%s\"\n", op->args.capture.name);
                str = closing;
                ++str;
            }
            vm_op_t *pat = parse(str);
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
            str = skip_spaces(str);
            vm_op_t *pat = NULL;
            if (*str != '~') {
                pat = parse(str);
                check(pat, "Expected pattern after '{'");
                pat = expand_choices(pat);
                str = pat->end;
                str = skip_spaces(str+1);
            }
            str = skip_spaces(str+1);
            char quote = *(str++);
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
            str = skip_spaces(str);
            check(*str == '}', "Expected a closing '}'");
            ++str;
            op->op = VM_REPLACE;
            op->args.replace.replace_pat = pat;
            op->args.replace.replacement = replacement;
            debug(" rep = \"%s\"", replacement);
            debug("}");
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

static void load_def(const char *name, const char *def)
{
    defs[ndefs].name = name;
    defs[ndefs].op = parse(def);
    ++ndefs;
}

static void load_defs(void)
{
    load_def("_", "` /\\t/\\n/\\r");
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

int main(int argc, char *argv[])
{
    load_defs();

    char *lang = argc > 1 ? argv[1] : "'x''y'";
    vm_op_t *op = parse(lang);
    check(op, "Failed to parse input");
    op = expand_choices(op);
    
    // TODO: check for semicolon and more rules


    char *str = argc > 2 ? argv[2] : "xyz";

    // Ensure string has a null byte to the left:
    char *lpadded = calloc(sizeof(char), strlen(str)+2);
    stpcpy(&lpadded[1], str);
    str = &lpadded[1];

    match_t *m = match(str, op);
    if (m == NULL) {
        printf("No match\n");
    } else {
        printf("%.*s\033[7m%.*s\033[0m%s\n",
               (int)(str - m->start), str,
               (int)(m->end - m->start), m->start, 
               m->end);
    }

    return 0;
}
