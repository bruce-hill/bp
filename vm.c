/*
 * vm.c - Code for the BP virtual machine that performs the matching.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "grammar.h"
#include "types.h"
#include "utils.h"
#include "vm.h"


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
    [VM_HIDE] = "HIDE",
    [VM_OTHERWISE] = "OTHERWISE",
    [VM_CHAIN] = "CHAIN",
    [VM_REPLACE] = "REPLACE",
    [VM_EQUAL] = "EQUAL",
    [VM_NOT_EQUAL] = "NOT_EQUAL",
    [VM_REF] = "REF",
    [VM_BACKREF] = "BACKREF",
    [VM_NODENT] = "NODENT",
};

// UTF8-compliant char iteration
static inline const char *next_char(file_t *f, const char *str)
{
    char c = *str;
    ++str;
    if (__builtin_expect(!(c & 0x80), 1))
        return str;

    if (__builtin_expect(str < f->end && !!(*str & 0x80), 1))
        ++str;
    if (c > '\xDF' && __builtin_expect(str < f->end && !!(*str & 0x80), 1))
        ++str;
    if (c > '\xEF' && __builtin_expect(str < f->end && !!(*str & 0x80), 1))
        ++str;
    return str;
}

const char *opcode_name(enum VMOpcode o)
{
    return opcode_names[o];
}

/*
 * Recursively deallocate a match object and set to NULL
 */
void destroy_match(match_t **m)
{
    if (!*m) return;
    destroy_match(&((*m)->child));
    destroy_match(&((*m)->nextsibling));
    *m = NULL;
}

typedef struct recursive_ref_s {
    const vm_op_t *op;
    const char *pos;
    struct recursive_ref_s *prev;
    int hit;
    match_t *result;
} recursive_ref_t;

/*
 * Attempt to match text against a previously captured value.
 */
static match_t *match_backref(const char *str, vm_op_t *op, match_t *cap, unsigned int flags)
{
    check(op->op == VM_BACKREF, "Attempt to match backref against something that's not a backref");
    match_t *ret = new(match_t);
    ret->start = str;
    ret->op = op;
    match_t **dest = &ret->child;

    if (cap->op->op == VM_REPLACE) {
        const char *text = cap->op->args.replace.text;
        const char *end = &text[cap->op->args.replace.len];
        for (const char *r = text; r < end; ) {
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
            match_t *value = get_capture(cap, &r);
            if (value != NULL) {
                *dest = match_backref(str, op, value, flags);
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
                if ((flags & BP_IGNORECASE) ? memicmp(str, prev, len) != 0
                                              : memcmp(str, prev, len) != 0) {
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
            if ((flags & BP_IGNORECASE) ? memicmp(str, prev, len) != 0
                                          : memcmp(str, prev, len) != 0) {
                destroy_match(&ret);
                return NULL;
            }
            str += len;
        }
    }
    ret->end = str;
    return ret;
}


/*
 * Run virtual machine operation against a string and return
 * a match struct, or NULL if no match is found.
 * The returned value should be free()'d to avoid memory leaking.
 */
static match_t *_match(def_t *defs, file_t *f, const char *str, vm_op_t *op, unsigned int flags, recursive_ref_t *rec)
{
    switch (op->op) {
        case VM_ANYCHAR: {
            if (str >= f->end || *str == '\n')
                return NULL;
            match_t *m = new(match_t);
            m->op = op;
            m->start = str;
            m->end = next_char(f, str);
            return m;
        }
        case VM_STRING: {
            if (&str[op->len] > f->end) return NULL;
            if ((flags & BP_IGNORECASE) ? memicmp(str, op->args.s, (size_t)op->len) != 0
                                          : memcmp(str, op->args.s, (size_t)op->len) != 0)
                return NULL;
            match_t *m = new(match_t);
            m->op = op;
            m->start = str;
            m->end = str + op->len;
            return m;
        }
        case VM_RANGE: {
            if (str >= f->end) return NULL;
            if ((unsigned char)*str < op->args.range.low || (unsigned char)*str > op->args.range.high)
                return NULL;
            match_t *m = new(match_t);
            m->op = op;
            m->start = str;
            m->end = str + 1;
            return m;
        }
        case VM_NOT: {
            match_t *m = _match(defs, f, str, op->args.pat, flags, rec);
            if (m != NULL) {
                destroy_match(&m);
                return NULL;
            }
            m = new(match_t);
            m->op = op;
            m->start = str;
            m->end = str;
            return m;
        }
        case VM_UPTO_AND: {
            match_t *m = new(match_t);
            m->start = str;
            m->op = op;
            if (!op->args.multiple.first && !op->args.multiple.second) {
                while (str < f->end && *str != '\n') ++str;
            } else {
                match_t **dest = &m->child;
                for (const char *prev = NULL; prev < str; ) {
                    prev = str;
                    if (op->args.multiple.first) {
                        match_t *p = _match(defs, f, str, op->args.multiple.first, flags, rec);
                        if (p) {
                            *dest = p;
                            m->end = p->end;
                            return m;
                        }
                    } else if (str == f->end) {
                        m->end = str;
                        return m;
                    }
                    if (op->args.multiple.second) {
                        match_t *p = _match(defs, f, str, op->args.multiple.second, flags, rec);
                        if (p) {
                            *dest = p;
                            dest = &p->nextsibling;
                            str = p->end;
                            continue;
                        }
                    }
                    // This isn't in the for() structure because there needs to
                    // be at least once chance to match the pattern, even if
                    // we're at the end of the string already (e.g. "..$").
                    if (str < f->end && *str != '\n')
                        str = next_char(f, str);
                }
                destroy_match(&m);
                return NULL;
            }
            m->end = str;
            return m;
        }
        case VM_REPEAT: {
            match_t *m = new(match_t);
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
                    sep = _match(defs, f, str, op->args.repetitions.sep, flags, rec);
                    if (sep == NULL) break;
                    str = sep->end;
                }
                match_t *p = _match(defs, f, str, op->args.repetitions.repeat_pat, flags, rec);
                if (p == NULL) {
                    destroy_match(&sep);
                    break;
                }
                if (p->end == start && reps > 0) {
                    // Since no forward progress was made on either `pat` or
                    // `sep` and BP does not have mutable state, it's
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
            match_t *before = _match(defs, f, str - backtrack, op->args.pat, flags, rec);
            if (before == NULL) return NULL;
            match_t *m = new(match_t);
            m->start = str;
            m->end = str;
            m->op = op;
            m->child = before;
            return m;
        }
        case VM_BEFORE: {
            match_t *after = _match(defs, f, str, op->args.pat, flags, rec);
            if (after == NULL) return NULL;
            match_t *m = new(match_t);
            m->start = str;
            m->end = str;
            m->op = op;
            m->child = after;
            return m;
        }
        case VM_CAPTURE: {
            match_t *p = _match(defs, f, str, op->args.pat, flags, rec);
            if (p == NULL) return NULL;
            match_t *m = new(match_t);
            m->start = str;
            m->end = p->end;
            m->op = op;
            m->child = p;
            return m;
        }
        case VM_HIDE: {
            match_t *p = _match(defs, f, str, op->args.pat, flags, rec);
            if (p == NULL) return NULL;
            match_t *m = new(match_t);
            m->start = str;
            m->end = p->end;
            m->op = op;
            m->child = p;
            return m;
        }
        case VM_OTHERWISE: {
            match_t *m = _match(defs, f, str, op->args.multiple.first, flags, rec);
            if (m == NULL) m = _match(defs, f, str, op->args.multiple.second, flags, rec);
            return m;
        }
        case VM_CHAIN: {
            match_t *m1 = _match(defs, f, str, op->args.multiple.first, flags, rec);
            if (m1 == NULL) return NULL;

            match_t *m2;
            { // Push backrefs and run matching, then cleanup
                def_t *defs2 = with_backrefs(defs, f, m1);
                m2 = _match(defs2, f, m1->end, op->args.multiple.second, flags, rec);
                while (defs2 != defs) {
                    def_t *next = defs2->next;
                    defs2->next = NULL;
                    xfree(&defs2);
                    defs2 = next;
                }
            }

            if (m2 == NULL) {
                destroy_match(&m1);
                return NULL;
            }
            match_t *m = new(match_t);
            m->start = str;
            m->end = m2->end;
            m->op = op;
            m->child = m1;
            m1->nextsibling = m2;
            return m;
        }
        case VM_EQUAL: case VM_NOT_EQUAL: {
            match_t *m1 = _match(defs, f, str, op->args.multiple.first, flags, rec);
            if (m1 == NULL) return NULL;

            // <p1>==<p2> matches iff the text of <p1> matches <p2>
            // <p1>!=<p2> matches iff the text of <p1> does not match <p2>
            file_t inner = {
                .filename=f->filename,
                .contents=(char*)m1->start, .end=(char*)m1->end,
                .lines=f->lines, // I think this works, but am not 100% sure
                .nlines=1 + get_line_number(f, m1->end)-get_line_number(f, m1->start),
                .mmapped=f->mmapped,
            };
            match_t *m2 = _match(defs, &inner, str, op->args.multiple.second, flags, rec);
            if ((m2 == NULL) == (op->op == VM_EQUAL)) {
                destroy_match(&m1);
                destroy_match(&m2);
                return NULL;
            }
            match_t *m = new(match_t);
            m->start = m1->start;
            m->end = m1->end;
            m->op = op;
            m->child = m1;
            if (op->op == VM_EQUAL) {
                m1->nextsibling = m2;
            } else {
                destroy_match(&m2);
            }
            return m;
        }
        case VM_REPLACE: {
            match_t *p = NULL;
            if (op->args.replace.pat) {
                p = _match(defs, f, str, op->args.replace.pat, flags, rec);
                if (p == NULL) return NULL;
            }
            match_t *m = new(match_t);
            m->start = str;
            m->op = op;
            if (p) {
                m->child = p;
                m->end = p->end;
            } else {
                m->end = m->start;
            }
            return m;
        }
        case VM_REF: {
            vm_op_t *r = lookup(defs, op->args.s);
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
            match_t *p = _match(defs, f, str, r, flags, &wrap);
            if (p == NULL) return best;
            if (wrap.hit && (best == NULL || p->end > best->end)) {
                best = p;
                wrap.hit = 0;
                wrap.result = p;
                goto left_recursive;
            } else if (best == NULL) {
                best = p;
            }
            match_t *m = new(match_t);
            m->start = best->start;
            m->end = best->end;
            m->op = op;
            m->child = best;
            return m;
        }
        case VM_BACKREF: {
            return match_backref(str, op, op->args.backref, flags);
        }
        case VM_NODENT: {
            if (*str != '\n') return NULL;
            const char *start = str;

            size_t linenum = get_line_number(f, str);
            const char *p = get_line(f, linenum);
            if (p < f->contents) p=f->contents; // Can happen with recursive matching

            // Current indentation:
            char denter = *p;
            int dents = 0;
            if (denter == ' ' || denter == '\t') {
                for (; *p == denter && p < f->end; ++p) ++dents;
            }

            // Subsequent indentation:
            while (*str == '\n') ++str;
            for (int i = 0; i < dents; i++) {
                if (str[i] != denter || &str[i] >= f->end) return NULL;
            }

            match_t *m = new(match_t);
            m->start = start;
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

/*
 * Get a specific numbered pattern capture.
 */
static match_t *get_capture_by_num(match_t *m, int *n)
{
    if (*n == 0) return m;
    if (m->op->op == VM_CAPTURE && *n == 1) return m;
    if (m->op->op == VM_CAPTURE) --(*n);
    for (match_t *c = m->child; c; c = c->nextsibling) {
        match_t *cap = get_capture_by_num(c, n);
        if (cap) return cap;
    }
    return NULL;
}

/*
 * Get a capture with a specific name.
 */
static match_t *get_capture_by_name(match_t *m, const char *name)
{
    if (m->op->op == VM_CAPTURE && m->op->args.capture.name
        && streq(m->op->args.capture.name, name))
        return m;
    for (match_t *c = m->child; c; c = c->nextsibling) {
        match_t *cap = get_capture_by_name(c, name);
        if (cap) return cap;
    }
    return NULL;
}

/*
 * Get a capture by name.
 */
match_t *get_capture(match_t *m, const char **r)
{
    if (isdigit(**r)) {
        int n = (int)strtol(*r, (char**)r, 10);
        return get_capture_by_num(m->child, &n);
    } else {
        const char *end = after_name(*r);
        if (end == *r) return NULL;
        char *name = strndup(*r, (size_t)(end-*r));
        match_t *cap = get_capture_by_name(m, name);
        xfree(&name);
        *r = end;
        if (**r == ';') ++(*r);
        return cap;
    }
    return NULL;
}

match_t *match(def_t *defs, file_t *f, const char *str, vm_op_t *op, unsigned int flags)
{
    return _match(defs, f, str, op, flags, NULL);
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
