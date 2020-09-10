/*
 * bpeg.c - Source code for the bpeg parser
 *
 * Grammar:
 *     # <comment>                 comment
 *     .                           any character (multiline: $.)
 *     ^                           beginning of a line (^^: beginning of file)
 *     $                           end of a line ($$: end of file)
 *     _                           0 or more spaces or tabs (__: include newlines and comments)
 *     `<c>                        character <c>
 *     `<a>-<z>                    character between <a> and <z>
 *     \<e>                        escape sequence (e.g. \n, \033)
 *     \<e1>-<e2>                  escape sequence range (e.g. \x00-\xF0)
 *     ! <pat>                     no <pat>
 *     ~ <pat>                     any character as long as it doesn't match <pat> (multiline: ~~<pat>)
 *     & <pat>                     upto and including <pat> (aka *~<pat> <pat>) (multiline: &&<pat>)
 *     <N=1> + <pat> [% <sep="">]  <N> or more <pat>s (separated by <sep>)
 *     * <pat> [% <sep="">]        sugar for "0+ <pat> [% <sep>]"
 *     <N=1> - <pat> [% <sep="">]  <N> or fewer <pat>s (separated by <sep>)
 *     ? <pat>                     sugar for "1- <pat>"
 *     <N> - <M> <pat>             <N> to <M> (inclusive) <pat>s
 *     < <pat>                     after <pat>, ...
 *     > <pat>                     before <pat>, ...
 *     ( <pat> )                   <pat>
 *     @ <pat>                     capture <pat>
 *     @ [ <name> ] <pat>          <pat> named <name>
 *     { <pat> => <str> }           <pat> replaced with <str>
 *     "@1" or "@[1]"              first capture
 *     "@foo" or "@[foo]"          capture named "foo"
 *     <pat1> <pat2>               <pat1> followed by <pat2>
 *     <pat> / <alt>               <pat> otherwise <alt>
 *     ; <name> = <pat>            <name> is defined to be <pat>
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
            if (!*str || (!op->multiline && *str == '\n'))
                return NULL;
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
        case VM_NOT: case VM_ANYTHING_BUT: {
            if (op->op == VM_ANYTHING_BUT)
                if (!*str || (!op->multiline && *str == '\n'))
                    return NULL;
            match_t *m = match(str, op->args.pat);
            if (m != NULL) {
                m = free_match(m);
                return NULL;
            }
            m = calloc(sizeof(match_t), 1);
            m->start = str;
            if (op->op == VM_ANYTHING_BUT) ++str;
            m->end = str;
            return m;
        }
        case VM_UPTO_AND: {
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            match_t *p = NULL;
            for (const char *prev = NULL; p == NULL && prev < str; ) {
                prev = str;
                p = match(str, op->args.pat);
                if (*str && (op->multiline || *str != '\n'))
                    ++str;
            }
            if (p) {
                m->end = p->end;
                m->child = p;
                return m;
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

            const char *prev = str;
            size_t reps;
            for (reps = 0; reps < (size_t)op->args.repetitions.max; ++reps) {
                // Separator
                match_t *sep = NULL;
                if (op->args.repetitions.sep != NULL && reps > 0) {
                    sep = match(str, op->args.repetitions.sep);
                    if (sep == NULL) break;
                    str = sep->end;
                }
                match_t *p = match(str, op->args.repetitions.repeat_pat);
                if (p == NULL || (p->end == prev && reps > 0)) { // Prevent infinite loops
                    if (sep) sep = free_match(sep);
                    if (p) p = free_match(p);
                    break;
                }
                if (sep) {
                    *dest = sep;
                    dest = &sep->nextsibling;
                }
                *dest = p;
                dest = &p->nextsibling;
                str = p->end;
                prev = str;
            }

            if ((ssize_t)reps < op->args.repetitions.min) {
                m = free_match(m);
                return NULL;
            }
            m->end = str;
            return m;
        }
        case VM_AFTER: {
            ssize_t backtrack = op->args.pat->len;
            check(backtrack != -1, "'<' is only allowed for fixed-length operations");
            // Check for necessary space:
            for (int i = 0; i < backtrack; i++) {
                if (str[-i] == '\0') return NULL;
            }
            match_t *before = match(str - backtrack, op->args.pat);
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
            // Search backwards so newer defs take precedence
            for (int i = ndefs-1; i >= 0; i--) {
                if (streq(defs[i].name, op->args.s)) {
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
 * Take an opcode and expand it into a chain of patterns if it's
 * followed by any patterns (e.g. "`x `y"), otherwise return
 * the original input.
 */
static vm_op_t *expand_chain(const char *source, vm_op_t *first)
{
    visualize(source, first->end, "Expanding chain...");
    vm_op_t *second = compile_bpeg(source, first->end);
    if (second == NULL) return first;
    second = expand_chain(source, second);
    check(second->end > first->end, "No forward progress in chain!");
    visualize(source, second->end, "Got chained pair.");
    return chain_together(first, second);
}

/*
 * Take an opcode and expand it into a chain of choices if it's
 * followed by any "/"-separated patterns (e.g. "`x/`y"), otherwise
 * return the original input.
 */
static vm_op_t *expand_choices(const char *source, vm_op_t *first)
{
    first = expand_chain(source, first);
    const char *str = first->end;
    if (!matchchar(&str, '/')) return first;
    visualize(source, str, "Expanding choices...");
    //debug("Otherwise:\n");
    vm_op_t *second = compile_bpeg(source, str);
    check(second, "Expected pattern after '/'");
    second = expand_choices(source, second);
    vm_op_t *choice = calloc(sizeof(vm_op_t), 1);
    choice->op = VM_OTHERWISE;
    choice->start = first->start;
    if (first->len == second->len)
        choice->len = first->len;
    else choice->len = -1;
    choice->end = second->end;
    choice->args.multiple.first = first;
    choice->args.multiple.second = second;
    visualize(source, choice->end, "Got two choices");
    return choice;
}

static char unescapechar(const char *escaped, const char **end)
{
    size_t len = 1;
    char ret = *escaped;
    switch (*escaped) {
        case 'a': ret = '\a'; break; case 'b': ret = '\b'; break;
        case 'n': ret = '\n'; break; case 'r': ret = '\r'; break;
        case 't': ret = '\t'; break; case 'v': ret = '\v'; break;
        case 'e': ret = '\033'; break;
        case 'x': { // Hex
            static const char hextable[255] = {
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
            ret = escaped[0] - '0';
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
        default: break;
    }
    *end = &escaped[len];
    return ret;
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
static vm_op_t *compile_bpeg(const char *source, const char *str)
{
    if (!*str) return NULL;
    visualize(source, str, "Compiling...");
    //debug("Parsing \"%s\"...\n", str);
    str = after_spaces(str);
    check(*str, "Expected a pattern");
    vm_op_t *op = calloc(sizeof(vm_op_t), 1);
    op->start = str;
    op->len = -1;
    char c = *str;
    ++str;
    switch (c) {
        // Any char (dot) ($. is multiline anychar)
        case '.': {
          anychar:
            visualize(source, str, "Dot");
            //debug("Dot\n");
            op->op = VM_ANYCHAR;
            op->len = 1;
            break;
        }
        // Char literals
        case '`': {
            char literal[2] = {*str, '\0'};
            ++str;
            visualize(source, str, "Char literal");
            check(literal[0], "Expected character after '`'\n");
            op->len = 1;
            if (matchchar(&str, '-')) { // Range
                visualize(source, str, "Char range");
                char c2 = *str;
                check(c2, "Expected character after '-'");
                check(c2 >= literal[0], "Character range must be low-to-high");
                op->op = VM_RANGE;
                op->args.range.low = literal[0];
                op->args.range.high = c2;
                ++str;
            } else {
                //debug("Char literal\n");
                op->op = VM_STRING;
                op->args.s = strdup(literal);
            }
            break;
        }
        // Escapes
        case '\\': {
            //debug("Escape sequence\n");
            visualize(source, str, "Escape sequence");
            check(*str, "Expected escape after '\\'");
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
            visualize(source, str, "String literal");
            char endquote = c == '\002' ? '\003' : c;
            char *literal = (char*)str;
            for (; *str && *str != endquote; str++) {
                if (*str == '\\') {
                    check(str[1], "Expected more string contents after backslash");
                    ++str;
                }
                visualize(source, str, "String literal");
            }
            size_t len = (size_t)(str - literal);
            literal = strndup(literal, len);
            len = unescape_string(literal, literal, len);

            op->op = VM_STRING;
            op->len = len;
            op->args.s = literal;

            check(matchchar(&str, endquote), "Missing closing quote");
            break;
        }
        // Not <pat>
        case '!': {
            // debug("Not pattern\n");
            visualize(source, str, "Not <pat>");
            vm_op_t *p = compile_bpeg(source, str);
            check(p, "Expected pattern after '!'\n");
            str = p->end;
            op->op = VM_NOT;
            op->len = 0;
            op->args.pat = p;
            break;
        }
        // Anything but <pat>
        case '~': {
            if (matchchar(&str, '~')) op->multiline = 1;
            visualize(source, str, "Anything but <pat>");
            vm_op_t *p = compile_bpeg(source, str);
            check(p, "Expected pattern after '~'\n");
            str = p->end;
            op->op = VM_ANYTHING_BUT;
            op->len = -1;
            op->args.pat = p;
            break;
        }
        // Upto and including <pat>
        case '&': {
            if (matchchar(&str, '&')) op->multiline = 1;
            visualize(source, str, "Upto and including <pat>");
            vm_op_t *p = compile_bpeg(source, str);
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
            visualize(source, str, "Repeat <pat>");
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
            visualize(source, str, NULL);
            vm_op_t *pat = compile_bpeg(source, str);
            check(pat, "Expected pattern after repetition count");
            str = pat->end;
            str = after_spaces(str);
            if (matchchar(&str, '%')) {
                visualize(source, str, "Repeat <pat> with separator");
                vm_op_t *sep = compile_bpeg(source, str);
                check(sep, "Expected pattern for separator after '%%'");
                str = sep->end;
                set_range(op, min, max, pat, sep);
            } else {
                set_range(op, min, max, pat, NULL);
            }
            visualize(source, str, NULL);
            //debug("min = %lld max = %lld\n", (long long)op->args.repetitions.min, (long long)op->args.repetitions.max);
            break;
        }
        // Special repetitions:
        case '+': case '*': case '?': {
            //debug("Special repetitions\n");
            visualize(source, str, "Repeat <pat>");
            ssize_t min = -1, max = -1;
            switch (c) {
                case '+': min = 1, max = -1; break;
                case '*': min = 0, max = -1; break;
                case '?': min = 0, max = 1; break;
            }
            vm_op_t *pat = compile_bpeg(source, str);
            check(pat, "Expected pattern after +");
            str = pat->end;
            str = after_spaces(str);
            if (matchchar(&str, '%')) {
                visualize(source, str, "Repeat <pat> with separator");
                vm_op_t *sep = compile_bpeg(source, str);
                check(sep, "Expected pattern for separator after '%%'");
                str = sep->end;
                set_range(op, min, max, pat, sep);
            } else {
                set_range(op, min, max, pat, NULL);
            }
            visualize(source, str, NULL);
            //debug("min = %lld max = %lld\n", (long long)op->args.repetitions.min, (long long)op->args.repetitions.max);
            break;
        }
        // Lookbehind
        case '<': {
            visualize(source, str, "After <pat>");
            //debug("Lookbehind\n");
            vm_op_t *pat = compile_bpeg(source, str);
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
            visualize(source, str, "Before <pat>");
            //debug("Lookahead\n");
            vm_op_t *pat = compile_bpeg(source, str);
            check(pat, "Expected pattern after >");
            str = pat->end;
            op->op = VM_BEFORE;
            op->len = 0;
            op->args.pat = pat;
            break;
        }
        // Parentheses
        case '(': {
            visualize(source, str, NULL);
            // debug("Open paren (\n");
            free(op);
            op = compile_bpeg(source, str);
            check(op, "Expected pattern inside parentheses");
            op = expand_choices(source, op);
            str = op->end;
            str = after_spaces(str);
            check(matchchar(&str, ')'), "Expected closing parenthesis");
            visualize(source, str, NULL);
            // debug(")\n");
            break;
        }
        // Capture
        case '@': {
            //debug("Capture\n");
            visualize(source, str, "Capture");
            op->op = VM_CAPTURE;
            str = after_spaces(str);
            if (matchchar(&str, '[')) {
                char *closing = strchr(str, ']');
                check(closing, "Expected closing ']'");
                op->args.capture.name = strndup(str, (size_t)(closing-str));
                visualize(source, str, "Named capture");
                //debug("named \"%s\"\n", op->args.capture.name);
                str = closing;
                check(matchchar(&str, ']'), "Expected closing ']'");
            }
            vm_op_t *pat = compile_bpeg(source, str);
            check(pat, "Expected pattern after @");
            str = pat->end;
            op->args.capture.capture_pat = pat;
            op->len = pat->len;
            visualize(source, str, NULL);
            break;
        }
        // Replacement
        case '{': {
            //debug("Replacement {\n");
            visualize(source, str, "Replacement");
            str = after_spaces(str);
            vm_op_t *pat = NULL;
            if (strncmp(str, "=>", 2) == 0) {
                str += strlen("=>");
            } else {
                pat = compile_bpeg(source, str);
                check(pat, "Invalid pattern after '{'");
                pat = expand_choices(source, pat);
                str = pat->end;
                str = after_spaces(str);
                check(matchchar(&str, '=') && matchchar(&str, '>'),
                      "Expected '=>' after pattern in replacement");
            }
            visualize(source, str, NULL);
            str = after_spaces(str);

            char quote = *str;
            const char *replacement;
            if (matchchar(&str, '}')) {
                replacement = strdup("");
                visualize(source, str, NULL);
            } else {
                check(matchchar(&str, '"') || matchchar(&str, '\''),
                      "Expected string literal for replacement");
                replacement = str;
                for (; *str && *str != quote; str++) {
                    if (*str == '\\') {
                        check(str[1], "Expected more string contents after backslash");
                        ++str;
                    }
                    visualize(source, str, NULL);
                }
                replacement = strndup(replacement, (size_t)(str-replacement));
                check(matchchar(&str, quote), "Expected closing quote");
                check(matchchar(&str, '}'), "Expected a closing '}'");
            }
            op->op = VM_REPLACE;
            op->args.replace.replace_pat = pat;
            op->args.replace.replacement = replacement;
            //debug(" rep = \"%s\"\n", replacement);
            //debug("}\n");
            if (pat != NULL) op->len = pat->len;
            visualize(source, str, NULL);
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
            op->op = VM_REF;
            visualize(source, str, op->args.s);
            break;
        }
        // Empty choice (/) or {/}
        case '/': {
            str = after_spaces(str);
            if (*str == ')' || *str == '}') {
                op->op = VM_EMPTY;
            } else {
                free(op);
                return NULL;
            }
            break;
        }
        default: {
            // Reference
            if (isalpha(c)) {
                visualize(source, str, "Ref");
                --str;
                const char *refname = str;
                str = after_name(str);
                op->op = VM_REF;
                op->len = (size_t)(str - refname);
                op->args.s = strndup(refname, op->len);
                break;
            } else {
                visualize(source, str, "Finished");
                free(op);
                return NULL;
            }
        }
    }
    op->end = str;
    return op;
}

/*
 * Similar to compile_bpeg, except that the pattern begins with an implicit, unclosable quote.
 */
static vm_op_t *compile_bpeg_string(const char *source, const char *str)
{
    visualize(source, str, "Compiling string...");
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
                interp = compile_bpeg(source, str + 1);
                check(interp != NULL, "No valid BPEG pattern detected after backslash");
                break;
            }
            visualize(source, str, "String literal");
        }
        // End of string
        size_t len = (size_t)(str - literal);
        literal = strndup(literal, len);
        len = unescape_string(literal, literal, len);
        strop->len = len;
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

static vm_op_t *compile_bpeg_replacement(vm_op_t *pat, const char *str)
{
    vm_op_t *op = calloc(sizeof(vm_op_t), 1);
    op->op = VM_REPLACE;
    op->start = pat->start;
    op->len = pat->len;
    op->args.replace.replace_pat = pat;
    const char *replacement = str;
    for (; *str; str++) {
        if (*str == '\\') {
            check(str[1], "Expected more string contents after backslash");
            ++str;
        }
    }
    replacement = strndup(replacement, (size_t)(str-replacement));
    op->args.replace.replacement = replacement;
    return op;
}

static vm_op_t *add_def(const char *name, const char *source, vm_op_t *op)
{
    check(ndefs < (sizeof(defs)/sizeof(defs[0])), "Too many definitions!");
    defs[ndefs].name = name;
    defs[ndefs].op = op;
    defs[ndefs].source = source;
    ++ndefs;
    return op;
}

static vm_op_t *load_def(const char *name, const char *source)
{
    vm_op_t *op = compile_bpeg(source, source);
    source = strndup((char*)op->start, (int)(op->end - op->start));
    op = expand_choices(source, op);
    return add_def(name, source, op);
}

static void load_defs(void)
{
    // Approximately these are in least-to-most used order so they will be
    // found most efficiently by the lookup, which goes in reverse order.
    load_def("crlf", "\\r\\n");
    load_def("cr", "\\r"); load_def("r", "\\r");
    load_def("anglebraces", "`< *(anglebraces / ~~`>) `>");
    load_def("brackets", "`[ *(brackets / ~~`]) `]");
    load_def("braces", "`{ *(braces / ~~`}) `}");
    load_def("parens", "`( *(parens / ~~`)) `)");
    load_def("id", "(`a-z/`A-Z/`_) *(`a-z/`A-Z/`_/`0-9)");
    load_def("HEX", "`0-9/`A-F");
    load_def("Hex", "`0-9/`a-f/`A-F");
    load_def("hex", "`0-9/`a-f");
    load_def("number", "+`0-9 ?(`. *`0-9) / `. +`0-9");
    load_def("digit", "`0-9");
    load_def("Abc", "`a-z/`A-Z");
    load_def("ABC", "`A-Z");
    load_def("abc", "`a-z");
    load_def("esc", "\\e"); load_def("e", "\\e");
    load_def("tab", "\\t"); load_def("t", "\\t");
    load_def("nl", "\\n"); load_def("lf", "\\n"); load_def("n", "\\n");
    load_def("c-block-comment", "'/*' &&'*/'");
    load_def("c-line-comment", "'//' &$");
    load_def("c-comment", "c-line-comment / c-block-comment");
    load_def("hash-comment", "`# &$");
    load_def("comment", "!(/)"); // undefined by default
    load_def("WS", "` /\\t/\\n/\\r/comment");
    load_def("ws", "` /\\t");
    load_def("$$", "!$.");
    load_def("$", "!.");
    load_def("^^", "!<$.");
    load_def("^", "!<.");
    load_def("__", "*(` /\\t/\\n/\\r/comment)");
    load_def("_", "*(` /\\t)");
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
    if (m->is_capture && m->name_or_replacement && streq(m->name_or_replacement, name))
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
        printf("\033[0;34m");
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
                        ++r;
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
                    print_match(cap, "\033[0;35m");
                    printf("\033[0;34m");
                }
            } else if (matchchar(&r, '\\')) {
                fputc(unescapechar(r, &r), stdout);
                --r;
            } else {
                fputc(*r, stdout);
            }
        }
    } else {
        if (m->is_capture && m->name_or_replacement)
          printf("\033[0;2;33m[%s:", m->name_or_replacement);
        const char *prev = m->start;
        for (match_t *child = m->child; child; child = child->nextsibling) {
            if (child->start > prev)
                printf("%s%.*s", color, (int)(child->start - prev), prev);
            print_match(child, m->is_capture ? "\033[0;1m" : color);
            prev = child->end;
        }
        if (m->end > prev)
            printf("%s%.*s", color, (int)(m->end - prev), prev);
        if (m->is_capture && m->name_or_replacement)
          printf("\033[0;2;33m]");
    }
}

/*
 * Read an entire file into memory.
 */
static char *readfile(int fd)
{
    size_t capacity = 1000, len = 0;
    char *buf = calloc(sizeof(char), capacity+1);
    ssize_t just_read;
    while ((just_read=read(fd, &buf[len], capacity-len)) > 0) {
        len += (size_t)just_read;
        if (len >= capacity)
            buf = realloc(buf, (capacity *= 2));
    }
    return buf;
}

static void print_grammar(vm_op_t *op)
{
    switch (op->op) {
        case VM_REF: fprintf(stderr, "a $%s", op->args.s); break;
        case VM_EMPTY: fprintf(stderr, "the empty string"); break;
        case VM_ANYCHAR: fprintf(stderr, "any char"); break;
        case VM_STRING: fprintf(stderr, "string \"%s\"", op->args.s); break;
        case VM_RANGE: {
            fprintf(stderr, "char from %c-%c", op->args.range.low, op->args.range.high);
            break;
        }
        case VM_REPEAT: {
            if (op->args.repetitions.max == -1)
                fprintf(stderr, "%ld or more (", op->args.repetitions.min);
            else
                fprintf(stderr, "%ld-%ld of (",
                        op->args.repetitions.min,
                        op->args.repetitions.max);
            print_grammar(op->args.repetitions.repeat_pat);
            fprintf(stderr, ")");
            if (op->args.repetitions.sep) {
                fprintf(stderr, " separated by (");
                print_grammar(op->args.repetitions.sep);
                fprintf(stderr, ")");
            }
            break;
        }
        case VM_NOT: {
            fprintf(stderr, "not (");
            print_grammar(op->args.pat);
            fprintf(stderr, ")");
            break;
        }
        case VM_UPTO_AND: {
            fprintf(stderr, "text up to and including (");
            print_grammar(op->args.pat);
            fprintf(stderr, ")");
            break;
        }
        case VM_ANYTHING_BUT: {
            fprintf(stderr, "anything but (");
            print_grammar(op->args.pat);
            fprintf(stderr, ")");
            break;
        }
        case VM_AFTER: {
            fprintf(stderr, "after (");
            print_grammar(op->args.pat);
            fprintf(stderr, ")");
            break;
        }
        case VM_BEFORE: {
            fprintf(stderr, "before (");
            print_grammar(op->args.pat);
            fprintf(stderr, ")");
            break;
        }
        case VM_CAPTURE: {
            fprintf(stderr, "capture (");
            print_grammar(op->args.pat);
            fprintf(stderr, ")");
            if (op->args.capture.name)
                fprintf(stderr, " and call it %s", op->args.capture.name);
            break;
        }
        case VM_OTHERWISE: {
            fprintf(stderr, "(");
            print_grammar(op->args.multiple.first);
            fprintf(stderr, ") or else ");
            if (op->args.multiple.second->op != VM_OTHERWISE)
                fprintf(stderr, "(");
            print_grammar(op->args.multiple.second);
            if (op->args.multiple.second->op != VM_OTHERWISE)
                fprintf(stderr, ")");
            break;
        }
        case VM_CHAIN: {
            fprintf(stderr, "(");
            print_grammar(op->args.multiple.first);
            fprintf(stderr, ") then ");
            if (op->args.multiple.second->op != VM_CHAIN)
                fprintf(stderr, "(");
            print_grammar(op->args.multiple.second);
            if (op->args.multiple.second->op != VM_CHAIN)
                fprintf(stderr, ")");
            break;
        }
        case VM_REPLACE: {
            fprintf(stderr, "replace ");
            if (op->args.replace.replace_pat) {
                fprintf(stderr, "(");
                print_grammar(op->args.replace.replace_pat);
                fprintf(stderr, ")");
            } else
                fprintf(stderr, "\"\"");
            fprintf(stderr, " with \"%s\"", op->args.replace.replacement);
            break;
        }
        default: break;
    }
}

static vm_op_t *load_grammar(const char *grammar)
{
    vm_op_t *op = compile_bpeg(grammar, grammar);
    check(op, "Failed to compile_bpeg input");
    op = expand_choices(grammar, op);
    
    const char *defs = op->end;
    while (matchchar(&defs, ';')) {
        if (verbose) fprintf(stderr, "\n");
        defs = after_spaces(defs);
        const char *name = defs;
        if (strncmp(name, "^^", 2) == 0 ||
            strncmp(name, "__", 2) == 0 ||
            strncmp(name, "$$", 2) == 0) {
            name = strndup(name, 2);
            defs += 2;
        } else if (*name == '^' || *name == '_' || *name == '$') {
            name = strndup(name, 1);
            defs += 1;
        } else {
            defs = after_name(defs);
            name = strndup(name, (size_t)(defs-name));
        }
        defs = after_spaces(defs);
        check(matchchar(&defs, '='), "Expected '=' in definition");
        vm_op_t *def = load_def(name, defs);
        check(def, "Couldn't load definition");
        defs = def->end;
    }
    return op;
}

int main(int argc, char *argv[])
{
    const char *pattern = NULL,
          *replacement = NULL,
          *grammarfile = NULL,
          *infile = NULL;

    for (int i = 1; i < argc; i++) {
        if (streq(argv[i], "--help") || streq(argv[i], "-h")) {
            printf("%s\n", usage);
            return 0;
        } else if (streq(argv[i], "--verbose") || streq(argv[i], "-v")) {
            verbose = 1;
        } else if (streq(argv[i], "--replace") || streq(argv[i], "-r")) {
            replacement = argv[++i];
        } else if (streq(argv[i], "--slow") || streq(argv[i], "-s")) {
            visualize_delay = 100000;
        } else if (streq(argv[i], "--grammar") || streq(argv[i], "-g")) {
            grammarfile = argv[++i];
        } else if (pattern == NULL) {
            pattern = argv[i];
        } else if (infile == NULL) {
            infile = argv[i];
        }
    }

    check(pattern != NULL || grammarfile != NULL, usage);
    if (verbose) fprintf(stderr, "====== Loading definitions ======\n\n\n\n");
    {
        int tmp1 = visualize_delay, tmp2 = verbose;
        visualize_delay = -1, verbose = 0;
        load_defs();
        visualize_delay = tmp1, verbose = tmp2;
    }
    if (verbose) fprintf(stderr, "========== Compiling ===========\n\n\n\n");

    vm_op_t *op;
    if (grammarfile) {
        // load grammar from a file (semicolon mode)
        char *grammar;
        if (streq(grammarfile, "-")) {
            grammar = readfile(STDIN_FILENO);
        } else {
            int fd = open(grammarfile, O_RDONLY);
            check(fd >= 0, "Couldn't open file: %s", argv[2]);
            grammar = readfile(fd);
        }
        op = load_grammar(grammar);
    } else {
        // load grammar in start-with-string mode:
        vm_op_t *pat = compile_bpeg_string(pattern, pattern);
        if (replacement) {
            pat = compile_bpeg_replacement(pat, replacement);
        }

        add_def("pattern", pattern, pat);

        const char *grammar = "*(@pattern / \\n / .)";
        op = compile_bpeg(grammar, grammar);
    }

    if (verbose) {
        fprintf(stderr, "\n\n");
        print_grammar(op);
        fprintf(stderr, "\n\n");
    }

    char *input;
    if (infile == NULL || streq(infile, "-")) {
        input = readfile(STDIN_FILENO);
    } else {
        int fd = open(infile, O_RDONLY);
        check(fd >= 0, "Couldn't open file: %s", argv[2]);
        input = readfile(fd);
    }

    // Ensure string has a null byte to the left:
    char *lpadded = calloc(sizeof(char), strlen(input)+2);
    stpcpy(&lpadded[1], input);
    input = &lpadded[1];

    match_t *m = match(input, op);
    if (m == NULL) {
        printf("No match\n");
        return 1;
    } else {
        print_match(m, "\033[0m");
        printf("\033[0;2m%s\n", m->end);
    }

    return 0;
}

//vim: ts=4
