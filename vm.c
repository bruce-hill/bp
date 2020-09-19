/*
 * vm.c - Code for the BPEG virtual machine that performs the matching.
 */

#include <ctype.h>

#include "vm.h"
#include "grammar.h"
#include "utils.h"

static match_t *match_backref(const char *str, vm_op_t *op, match_t *m, unsigned int flags);
static size_t push_backrefs(grammar_t *g, match_t *m);
static match_t *get_capture_n(match_t *m, int *n);
static match_t *get_capture_named(match_t *m, const char *name);

/*
 * The names of the opcodes (keep in sync with the enum definition above)
 */
static const char *opcode_names[] = {
    [VM_ANYCHAR] = "ANYCHAR",
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
    [VM_EQUAL] = "EQUAL",
    [VM_REF] = "REF",
    [VM_BACKREF] = "BACKREF",
    [VM_NODENT] = "NODENT",
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
    if (m->op->op == VM_CAPTURE && m->value.name) {
        ++count;
        push_backref(g, m->value.name, m->child);
    }
    if (m->child) count += push_backrefs(g, m->child);
    if (m->nextsibling) count += push_backrefs(g, m->nextsibling);
    return count;
}

typedef struct recursive_ref_s {
    const vm_op_t *op;
    const char *pos;
    struct recursive_ref_s *prev;
    int hit;
    match_t *result;
} recursive_ref_t;


/*
 * Run virtual machine operation against a string and return
 * a match struct, or NULL if no match is found.
 * The returned value should be free()'d to avoid memory leaking.
 */
static match_t *_match(grammar_t *g, file_t *f, const char *str, vm_op_t *op, unsigned int flags, recursive_ref_t *rec)
{
    switch (op->op) {
        case VM_ANYCHAR: {
            if (str >= f->end - 1 || (!op->multiline && *str == '\n'))
                return NULL;
            match_t *m = calloc(sizeof(match_t), 1);
            m->op = op;
            m->start = str;
            m->end = str+1;
            return m;
        }
        case VM_STRING: {
            if ((flags & BPEG_IGNORECASE) ? strncasecmp(str, op->args.s, (size_t)op->len) != 0
                                          : strncmp(str, op->args.s, (size_t)op->len) != 0)
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
        case VM_NOT: {
            match_t *m = _match(g, f, str, op->args.pat, flags, rec);
            if (m != NULL) {
                destroy_match(&m);
                return NULL;
            }
            m = calloc(sizeof(match_t), 1);
            m->op = op;
            m->start = str;
            m->end = str;
            return m;
        }
        case VM_UPTO_AND: {
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->op = op;
            if (op->args.pat) {
                for (const char *prev = NULL; prev < str; ) {
                    prev = str;
                    match_t *p = _match(g, f, str, op->args.pat, flags, rec);
                    if (p) {
                        m->child = p;
                        m->end = p->end;
                        return m;
                    }
                    // This isn't in the for() structure because there needs to
                    // be at least once chance to match the pattern, even if
                    // we're at the end of the string already (e.g. "..$").
                    if (str < f->end && (op->multiline || *str != '\n')) ++str;
                }
                destroy_match(&m);
                return NULL;
            } else if (op->multiline) {
                str = f->end;
            } else {
                while (str < f->end && *str != '\n') ++str;
            }
            m->end = str;
            return m;
        }
        case VM_REPEAT: {
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = str;
            m->op = op;

            match_t **dest = &m->child;
            size_t reps = 0;
            ssize_t max = op->args.repetitions.max;
            for (reps = 0; max == -1 || reps < (size_t)max; ++reps) {
                const char *start = str;
                // Separator
                match_t *sep = NULL;
                if (op->args.repetitions.sep != NULL && reps > 0) {
                    sep = _match(g, f, str, op->args.repetitions.sep, flags, rec);
                    if (sep == NULL) break;
                    str = sep->end;
                }
                match_t *p = _match(g, f, str, op->args.repetitions.repeat_pat, flags, rec);
                if (p == NULL) {
                    destroy_match(&sep);
                    break;
                }
                if (p->end == start && reps > 0) {
                    // Since no forward progress was made on either `pat` or
                    // `sep` and BPEG does not have mutable state, it's
                    // guaranteed that no progress will be made on the next
                    // loop either. We know that this will continue to loop
                    // until reps==max, so let's just cut to the chase instead
                    // of looping infinitely.
                    destroy_match(&sep);
                    destroy_match(&p);
                    if (op->args.repetitions.max == -1)
                        reps = ~(size_t)0;
                    else
                        reps = (size_t)op->args.repetitions.max;
                    break;
                }
                if (sep) {
                    *dest = sep;
                    dest = &sep->nextsibling;
                }
                *dest = p;
                dest = &p->nextsibling;
                str = p->end;
            }

            if (reps < (size_t)op->args.repetitions.min) {
                destroy_match(&m);
                return NULL;
            }
            m->end = str;
            return m;
        }
        case VM_AFTER: {
            ssize_t backtrack = op->args.pat->len;
            check(backtrack != -1, "'<' is only allowed for fixed-length operations");
            if (str - backtrack < f->contents) return NULL;
            match_t *before = _match(g, f, str - backtrack, op->args.pat, flags, rec);
            if (before == NULL) return NULL;
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = str;
            m->op = op;
            m->child = before;
            return m;
        }
        case VM_BEFORE: {
            match_t *after = _match(g, f, str, op->args.pat, flags, rec);
            if (after == NULL) return NULL;
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = str;
            m->op = op;
            m->child = after;
            return m;
        }
        case VM_CAPTURE: {
            match_t *p = _match(g, f, str, op->args.pat, flags, rec);
            if (p == NULL) return NULL;
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = p->end;
            m->op = op;
            m->child = p;
            if (op->args.capture.name)
                m->value.name = op->args.capture.name;
            return m;
        }
        case VM_OTHERWISE: {
            match_t *m = _match(g, f, str, op->args.multiple.first, flags, rec);
            if (m == NULL) m = _match(g, f, str, op->args.multiple.second, flags, rec);
            return m;
        }
        case VM_CHAIN: {
            match_t *m1 = _match(g, f, str, op->args.multiple.first, flags, rec);
            if (m1 == NULL) return NULL;

            size_t nbackrefs = push_backrefs(g, m1);
            match_t *m2 = _match(g, f, m1->end, op->args.multiple.second, flags, rec);
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
        case VM_EQUAL: {
            match_t *m1 = _match(g, f, str, op->args.multiple.first, flags, rec);
            if (m1 == NULL) return NULL;

            // <p1>==<p2> matches iff both have the same start and end point:
            match_t *m2 = _match(g, f, str, op->args.multiple.second, flags, rec);
            if (m2 == NULL || m2->end != m1->end) {
                destroy_match(&m1);
                destroy_match(&m2);
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
            match_t *p = NULL;
            if (op->args.replace.replace_pat) {
                p = _match(g, f, str, op->args.replace.replace_pat, flags, rec);
                if (p == NULL) return NULL;
            }
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->op = op;
            if (p) {
                m->child = p;
                m->end = p->end;
            } else {
                m->end = m->start;
            }
            m->value.replacement = op->args.replace.replacement;
            return m;
        }
        case VM_REF: {
            vm_op_t *r = lookup(g, op->args.s);
            check(r != NULL, "Unknown identifier: '%s'", op->args.s);

            // Prevent infinite left recursion:
            for (recursive_ref_t *p = rec; p; p = p->prev) {
                if (p->pos == str && p->op == r) {
                    ++p->hit;
                    return p->result;
                }
            }

            recursive_ref_t wrap = {
                .op = r,
                .pos = str,
                .prev = rec,
                .hit = 0,
                .result = NULL,
            };
            match_t *best = NULL;
          left_recursive:;
            match_t *p = _match(g, f, str, r, flags, &wrap);
            if (p == NULL) return best;
            if (wrap.hit && (best == NULL || p->end > best->end)) {
                best = p;
                wrap.hit = 0;
                wrap.result = p;
                goto left_recursive;
            } else if (best == NULL) {
                best = p;
            }
            match_t *m = calloc(sizeof(match_t), 1);
            m->start = best->start;
            m->end = best->end;
            m->op = op;
            m->child = best;
            m->value.name = op->args.s;
            return m;
        }
        case VM_BACKREF: {
            return match_backref(str, op, (match_t*)op->args.backref, flags);
        }
        case VM_NODENT: {
            size_t linenum = get_line_number(f, str);
            if (linenum == 1) { // First line
                if (str > f->contents)
                    return NULL;
                match_t *m = calloc(sizeof(match_t), 1);
                m->start = str;
                m->end = str;
                m->op = op;
                return m;
            } else if (str[-1] != '\n') {
                return NULL; // Not at beginning of line
            }
            const char *p = get_line(f, linenum - 1);
            // Count indentation:
            char denter = *p;
            int dents = 0;
            if (denter == ' ' || denter == '\t') {
                for (; *p == denter; ++p) ++dents;
            }
            for (int i = 0; i < dents; i++) {
                if (str[i] != denter) return NULL;
            }

            match_t *m = calloc(sizeof(match_t), 1);
            m->start = str;
            m->end = &str[dents];
            m->op = op;
            return m;
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
        case VM_NODENT: {
            fprintf(stderr, "the start of a line with the same indentation as the previous line");
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
    if (m->op->op == VM_CAPTURE && *n == 1) return m;
    if (m->op->op == VM_CAPTURE) --(*n);
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
    if (m->op->op == VM_CAPTURE && m->value.name && streq(m->value.name, name))
        return m;
    for (match_t *c = m->child; c; c = c->nextsibling) {
        match_t *cap = get_capture_named(c, name);
        if (cap) return cap;
    }
    return NULL;
}

static match_t *get_cap(match_t *m, const char **r)
{
    if (isdigit(**r)) {
        int n = (int)strtol(*r, (char**)r, 10);
        return get_capture_n(m->child, &n);
    } else if (**r == '[') {
        char *closing = strchr(*r+1, ']');
        if (!closing) return NULL;
        ++(*r);
        char *name = strndup(*r, (size_t)(closing-*r));
        match_t *cap = get_capture_named(m, name);
        free(name);
        *r = closing + 1;
        return cap;
    }
    return NULL;
}

/*
 * Print a match with replacements and highlighting.
 */
void print_match(file_t *f, match_t *m)
{
    if (m->op->op == VM_REPLACE) {
        for (const char *r = m->value.replacement; *r; ) {
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
            if (*r == '@') {
                fputc('@', stdout);
                continue;
            }
            if (*r == '#') {
                ++r;
                printf("%ld", get_line_number(f, m->start));
                continue;
            } else if (*r == ':') {
                ++r;
                printf("%ld", get_char_number(f, m->start));
                continue;
            } else if (*r == '&') {
                ++r;
                printf("%s", f->filename ? f->filename : "-");
                continue;
            }
            match_t *cap = get_cap(m, &r);
            if (cap != NULL) {
                print_match(f, cap);
            } else {
                fputc('@', stdout);
            }
        }
    } else {
        const char *prev = m->start;
        for (match_t *child = m->child; child; child = child->nextsibling) {
            // Skip children from e.g. zero-width matches like >@foo
            if (!(prev <= child->start && child->start <= m->end &&
                  prev <= child->end && child->end <= m->end))
                continue;
            if (child->start > prev)
                printf("%.*s", (int)(child->start - prev), prev);
            print_match(f, child);
            prev = child->end;
        }
        if (m->end > prev)
            printf("%.*s", (int)(m->end - prev), prev);
    }
}

static match_t *match_backref(const char *str, vm_op_t *op, match_t *cap, unsigned int flags)
{
    check(op->op == VM_BACKREF, "Attempt to match backref against something that's not a backref");
    match_t *ret = calloc(sizeof(match_t), 1);
    ret->start = str;
    ret->op = op;
    match_t **dest = &ret->child;

    if (cap->op->op == VM_REPLACE) {
        for (const char *r = cap->value.replacement; *r; ) {
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
                *dest = match_backref(str, op, cap, flags);
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
                if ((flags & BPEG_IGNORECASE) ? strncasecmp(str, prev, len) != 0
                                              : strncmp(str, prev, len) != 0) {
                    destroy_match(&ret);
                    return NULL;
                }
                str += len;
                prev = child->start;
            }
            if (child->start < prev) continue;
            *dest = match_backref(str, op, child, flags);
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
            if ((flags & BPEG_IGNORECASE) ? strncasecmp(str, prev, len) != 0
                                          : strncmp(str, prev, len) != 0) {
                destroy_match(&ret);
                return NULL;
            }
            str += len;
        }
    }
    ret->end = str;
    return ret;
}

match_t *match(grammar_t *g, file_t *f, const char *str, vm_op_t *op, unsigned int flags)
{
    return _match(g, f, str, op, flags, NULL);
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
