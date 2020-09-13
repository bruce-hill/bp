/*
 * vm.c - Code for the BPEG virtual machine that performs the matching.
 */
#include "vm.h"
#include "grammar.h"
#include "utils.h"

static match_t *match_backref(const char *str, vm_op_t *op, match_t *m);
static size_t push_backrefs(grammar_t *g, match_t *m);
static match_t *get_capture_n(match_t *m, int *n);
static match_t *get_capture_named(match_t *m, const char *name);

/*
 * The names of the opcodes (keep in sync with the enum definition above)
 */
static const char *opcode_names[] = {
    [VM_EMPTY] = "EMPTY",
    [VM_ANYCHAR] = "ANYCHAR",
    [VM_ANYTHING_BUT] = "ANYTHING_BUT",
    [VM_STRING] = "STRING",
    [VM_RANGE] = "RANGE",
    [VM_NOT] = "NOT",
    [VM_UPTO_AND] = "UPTO_AND",
    [VM_REPEAT] = "REPEAT",
    [VM_BEFORE] = "BEFORE",
    [VM_AFTER] = "AFTER",
    [VM_CAPTURE] = "CAPTURE",
    [VM_OTHERWISE] = "OTHERWISE",
    [VM_CHAIN] = "CHAIN",
    [VM_REPLACE] = "REPLACE",
    [VM_REF] = "REF",
    [VM_BACKREF] = "BACKREF",
};

const char *opcode_name(enum VMOpcode o)
{
    return opcode_names[o];
}

/*
 * Recursively deallocate a match object and set to NULL
 */
void destroy_match(match_t **m)
{
    if (!m || !*m) return;
    destroy_match(&((*m)->child));
    destroy_match(&((*m)->nextsibling));
    *m = NULL;
}

static size_t push_backrefs(grammar_t *g, match_t *m)
{
    if (m == NULL) return 0;
    if (m->op->op == VM_REF) return 0;
    size_t count = 0;
    if (m->is_capture && m->name_or_replacement) {
        ++count;
        push_backref(g, m->name_or_replacement, m->child);
    }
    if (m->child) count += push_backrefs(g, m->child);
    if (m->nextsibling) count += push_backrefs(g, m->nextsibling);
    return count;
}

/*
 * Run virtual machine operation against a string and return
 * a match struct, or NULL if no match is found.
 * The returned value should be free()'d to avoid memory leaking.
 */
match_t *match(grammar_t *g, const char *str, vm_op_t *op)
{
  //tailcall:
    switch (op->op) {
        case VM_EMPTY: {
            match_t *m = calloc(sizeof(match_t), 1);
            m->op = op;
            m->start = str;
            m->end = str;
            return m;
        }
        case VM_ANYCHAR: {
            if (!*str || (!op->multiline && *str == '\n'))
                return NULL;
            match_t *m = calloc(sizeof(match_t), 1);
            m->op = op;
            m->start = str;
            m->end = str+1;
            return m;
        }
        case VM_STRING: {
            if (strncmp(str, op->args.s, (size_t)op->len) != 0)
                return NULL;
            match_t *m = calloc(sizeof(match_t), 1);
            m->op = op;
            m->start = str;
            m->end = str + op->len;
            return m;
        }
        case VM_RANGE: {
            if (*str < op->args.range.low || *str > op->args.range.high)
                return NULL;
            match_t *m = calloc(sizeof(match_t), 1);
            m->op = op;
            m->start = str;
            m->end = str + 1;
            return m;
        }
        case VM_NOT: case VM_ANYTHING_BUT: {
            if (op->op == VM_ANYTHING_BUT)
                if (!*str || (!op->multiline && *str == '\n'))
                    return NULL;
            match_t *m = match(g, str, op->args.pat);
            if (m != NULL) {
                destroy_match(&m);
                return NULL;
            }
            m = calloc(sizeof(match_t), 1);
            m->op = op;
            m->start = str;
            if (op->op == VM_ANYTHING_BUT) ++str;
            m->end = str;
            return m;
        }
        case VM_UPTO_AND: {
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->op = op;
            match_t *p = NULL;
            for (const char *prev = NULL; p == NULL && prev < str; ) {
                prev = str;
                p = match(g, str, op->args.pat);
                if (*str && (op->multiline || *str != '\n'))
                    ++str;
            }
            if (p) {
                m->end = p->end;
                m->child = p;
                return m;
            }
            destroy_match(&m);
            return NULL;
        }
        case VM_REPEAT: {
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = str;
            m->op = op;
            if (op->args.repetitions.max == 0) return m;

            match_t **dest = &m->child;

            const char *prev = str;
            size_t reps;
            for (reps = 0; reps < (size_t)op->args.repetitions.max; ++reps) {
                // Separator
                match_t *sep = NULL;
                if (op->args.repetitions.sep != NULL && reps > 0) {
                    sep = match(g, str, op->args.repetitions.sep);
                    if (sep == NULL) break;
                    str = sep->end;
                }
                match_t *p = match(g, str, op->args.repetitions.repeat_pat);
                if (p == NULL || (p->end == prev && reps > 0)) { // Prevent infinite loops
                    destroy_match(&sep);
                    destroy_match(&p);
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
                destroy_match(&m);
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
            match_t *before = match(g, str - backtrack, op->args.pat);
            if (before == NULL) return NULL;
            destroy_match(&before);
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = str;
            m->op = op;
            return m;
        }
        case VM_BEFORE: {
            match_t *after = match(g, str, op->args.pat);
            if (after == NULL) return NULL;
            destroy_match(&after);
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = str;
            m->op = op;
            return m;
        }
        case VM_CAPTURE: {
            match_t *p = match(g, str, op->args.pat);
            if (p == NULL) return NULL;
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = p->end;
            m->op = op;
            m->child = p;
            m->is_capture = 1;
            if (op->args.capture.name)
                m->name_or_replacement = op->args.capture.name;
            return m;
        }
        case VM_OTHERWISE: {
            match_t *m = match(g, str, op->args.multiple.first);
            if (m == NULL) m = match(g, str, op->args.multiple.second);
            return m;
        }
        case VM_CHAIN: {
            match_t *m1 = match(g, str, op->args.multiple.first);
            if (m1 == NULL) return NULL;

            size_t nbackrefs = push_backrefs(g, m1);
            match_t *m2 = match(g, m1->end, op->args.multiple.second);
            pop_backrefs(g, nbackrefs);
            if (m2 == NULL) {
                destroy_match(&m1);
                return NULL;
            }
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = m2->end;
            m->op = op;
            m->child = m1;
            m1->nextsibling = m2;
            return m;
        }
        case VM_REPLACE: {
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->op = op;
            if (op->args.replace.replace_pat) {
                match_t *p = match(g, str, op->args.replace.replace_pat);
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
            vm_op_t *r = lookup(g, op->args.s);
            check(r != NULL, "Unknown identifier: '%s'", op->args.s);
            // Could use tailcall here, but current have disabled
            match_t *p = match(g, str, r);
            if (p == NULL) return NULL;
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = p->start;
            m->end = p->end;
            m->op = op;
            m->child = p;
            m->name_or_replacement = op->args.s;
            m->is_ref = 1;
            return m;
        }
        case VM_BACKREF: {
            return match_backref(str, op, (match_t*)op->args.backref);
        }
        default: {
            fprintf(stderr, "Unknown opcode: %d", op->op);
            _exit(1);
            return NULL;
        }
    }
}


void print_pattern(vm_op_t *op)
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
            print_pattern(op->args.repetitions.repeat_pat);
            fprintf(stderr, ")");
            if (op->args.repetitions.sep) {
                fprintf(stderr, " separated by (");
                print_pattern(op->args.repetitions.sep);
                fprintf(stderr, ")");
            }
            break;
        }
        case VM_NOT: {
            fprintf(stderr, "not (");
            print_pattern(op->args.pat);
            fprintf(stderr, ")");
            break;
        }
        case VM_UPTO_AND: {
            fprintf(stderr, "text up to and including (");
            print_pattern(op->args.pat);
            fprintf(stderr, ")");
            break;
        }
        case VM_ANYTHING_BUT: {
            fprintf(stderr, "anything but (");
            print_pattern(op->args.pat);
            fprintf(stderr, ")");
            break;
        }
        case VM_AFTER: {
            fprintf(stderr, "after (");
            print_pattern(op->args.pat);
            fprintf(stderr, ")");
            break;
        }
        case VM_BEFORE: {
            fprintf(stderr, "before (");
            print_pattern(op->args.pat);
            fprintf(stderr, ")");
            break;
        }
        case VM_CAPTURE: {
            fprintf(stderr, "capture (");
            print_pattern(op->args.pat);
            fprintf(stderr, ")");
            if (op->args.capture.name)
                fprintf(stderr, " and call it %s", op->args.capture.name);
            break;
        }
        case VM_OTHERWISE: {
            fprintf(stderr, "(");
            print_pattern(op->args.multiple.first);
            fprintf(stderr, ") or else ");
            if (op->args.multiple.second->op != VM_OTHERWISE)
                fprintf(stderr, "(");
            print_pattern(op->args.multiple.second);
            if (op->args.multiple.second->op != VM_OTHERWISE)
                fprintf(stderr, ")");
            break;
        }
        case VM_CHAIN: {
            fprintf(stderr, "(");
            print_pattern(op->args.multiple.first);
            fprintf(stderr, ") then ");
            if (op->args.multiple.second->op != VM_CHAIN)
                fprintf(stderr, "(");
            print_pattern(op->args.multiple.second);
            if (op->args.multiple.second->op != VM_CHAIN)
                fprintf(stderr, ")");
            break;
        }
        case VM_REPLACE: {
            fprintf(stderr, "replace ");
            if (op->args.replace.replace_pat) {
                fprintf(stderr, "(");
                print_pattern(op->args.replace.replace_pat);
                fprintf(stderr, ")");
            } else
                fprintf(stderr, "\"\"");
            fprintf(stderr, " with \"%s\"", op->args.replace.replacement);
            break;
        }
        default: break;
    }
}

/*
 * Get a specific numbered pattern capture.
 */
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

/*
 * Get a named capture.
 */
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

/*
 * Print a match with replacements and highlighting.
 */
void print_match(match_t *m, const char *color, int verbose)
{
    if (m->is_replacement) {
        printf("\033[0;34m");
        for (const char *r = m->name_or_replacement; *r; ) {
            if (*r == '\\') {
                ++r;
                fputc(unescapechar(r, &r), stdout);
                continue;
            } else if (*r != '@') {
                fputc(*r, stdout);
                ++r;
                continue;
            }

            ++r;
            match_t *cap = NULL;
            switch (*r) {
                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9': {
                    int n = (int)strtol(r, (char**)&r, 10);
                    cap = get_capture_n(m->child, &n);
                    break;
                }
                case '[': {
                    char *closing = strchr(r+1, ']');
                    if (!closing) {
                        fputc('@', stdout);
                        break;
                    }
                    ++r;
                    char *name = strndup(r, (size_t)(closing-r));
                    cap = get_capture_named(m, name);
                    free(name);
                    r = closing + 1;
                    break;
                }
                default: {
                    fputc('@', stdout);
                    break;
                }
            }
            if (cap != NULL) {
                print_match(cap, "\033[0;35m", verbose);
                printf("\033[0;34m");
            }
        }
    } else {
        const char *name = m->name_or_replacement;
        if (verbose && m->is_ref && name && isupper(name[0]))
            printf("\033[0;2;35m{%s:", name);
        //if (m->is_capture && name)
        //    printf("\033[0;2;33m[%s:", name);
        const char *prev = m->start;
        for (match_t *child = m->child; child; child = child->nextsibling) {
            if (child->start > prev)
                printf("%s%.*s", color, (int)(child->start - prev), prev);
            print_match(child, m->is_capture ? "\033[0;1m" : color, verbose);
            prev = child->end;
        }
        if (m->end > prev)
            printf("%s%.*s", color, (int)(m->end - prev), prev);
        if (verbose && m->is_ref && name && isupper(name[0]))
            printf("\033[0;2;35m}");
        //if (m->is_capture && name)
        //    printf("\033[0;2;33m]");
    }
}

static match_t *match_backref(const char *str, vm_op_t *op, match_t *cap)
{
    check(op->op == VM_BACKREF, "Attempt to match backref against something that's not a backref");
    match_t *ret = calloc(sizeof(match_t), 1);
    ret->start = str;
    ret->op = op;
    match_t **dest = &ret->child;

    if (cap->is_replacement) {
        for (const char *r = cap->name_or_replacement; *r; ) {
            if (*r == '\\') {
                ++r;
                if (*(str++) != unescapechar(r, &r)) {
                    destroy_match(&ret);
                    return NULL;
                }
            } else if (*r != '@') {
                if (*(str++) != *r) {
                    destroy_match(&ret);
                    return NULL;
                }
                ++r;
                continue;
            }

            ++r;
            match_t *cap = NULL;
            switch (*r) {
                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9': {
                    int n = (int)strtol(r, (char**)&r, 10);
                    cap = get_capture_n(cap->child, &n);
                    break;
                }
                case '[': {
                    char *closing = strchr(r+1, ']');
                    if (!closing) {
                        if (*(str++) != '@') {
                            destroy_match(&ret);
                            return NULL;
                        }
                    }
                    ++r;
                    char *name = strndup(r, (size_t)(closing-r));
                    cap = get_capture_named(cap, name);
                    free(name);
                    r = closing + 1;
                    break;
                }
                default: {
                    if (*(str++) != '@') {
                        destroy_match(&ret);
                        return NULL;
                    }
                    break;
                }
            }
            if (cap != NULL) {
                *dest = match_backref(str, op, cap);
                if (*dest == NULL) {
                    destroy_match(&ret);
                    return NULL;
                }
                str = (*dest)->end;
                dest = &(*dest)->nextsibling;
            }
        }
    } else {
        const char *prev = cap->start;
        for (match_t *child = cap->child; child; child = child->nextsibling) {
            if (child->start > prev) {
                size_t len = (size_t)(child->start - prev);
                if (strncmp(str, prev, len) != 0) {
                    destroy_match(&ret);
                    return NULL;
                }
                str += len;
                prev = child->start;
            }
            *dest = match_backref(str, op, child);
            if (*dest == NULL) {
                destroy_match(&ret);
                return NULL;
            }
            str = (*dest)->end;
            dest = &(*dest)->nextsibling;
            prev = child->end;
        }
        if (cap->end > prev) {
            size_t len = (size_t)(cap->end - prev);
            if (strncmp(str, prev, len) != 0) {
                destroy_match(&ret);
                return NULL;
            }
            str += len;
        }
    }
    ret->end = str;
    return ret;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
