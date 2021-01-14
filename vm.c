//
// vm.c - Code for the BP virtual machine that performs the matching.
//

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "grammar.h"
#include "types.h"
#include "utils.h"
#include "vm.h"

__attribute__((nonnull, pure))
static inline const char *next_char(file_t *f, const char *str);
__attribute__((nonnull))
static const char *match_backref(const char *str, vm_op_t *op, match_t *cap, unsigned int flags);
__attribute__((hot, nonnull(2,3,4)))
static match_t *_match(def_t *defs, file_t *f, const char *str, vm_op_t *op, unsigned int flags);
__attribute__((nonnull))
static match_t *get_capture_by_num(match_t *m, int *n);
__attribute__((nonnull, pure))
static match_t *get_capture_by_name(match_t *m, const char *name);

static inline void set_owner(match_t *m, match_t **owner)
{
    check(owner != &m->child, "Circular ownership");
    check(owner != &m->nextsibling, "Circular ownership");
    *owner = m;
    ++m->refcount;
}

//
// Return the location of the next character or UTF8 codepoint.
// (i.e. skip forward one codepoint at a time, not one byte at a time)
//
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

//
// Recursively deallocate a match object and set to NULL
//
void destroy_match(match_t **m)
{
    if (*m == NULL) return;
    if (--(*m)->refcount > 0) {
        *m = NULL;
        return;
    }
    destroy_match(&((*m)->child));
    destroy_match(&((*m)->nextsibling));
    free(*m);
    *m = NULL;
}

//
// Attempt to match text against a previously captured value.
// Return the character position after the backref has matched, or NULL if no match has occurred.
//
static const char *match_backref(const char *str, vm_op_t *op, match_t *cap, unsigned int flags)
{
    check(op->type == VM_BACKREF, "Attempt to match backref against something that's not a backref");
    if (cap->op->type == VM_REPLACE) {
        const char *text = cap->op->args.replace.text;
        const char *end = &text[cap->op->args.replace.len];
        for (const char *r = text; r < end; ) {
            if (*r == '\\') {
                ++r;
                if (*(str++) != unescapechar(r, &r))
                    return NULL;
            } else if (*r != '@') {
                if (*(str++) != *r)
                    return NULL;
                ++r;
                continue;
            }

            ++r;
            match_t *value = get_capture(cap, &r);
            if (value != NULL) {
                str = match_backref(str, op, value, flags);
                if (str == NULL) return NULL;
            }
        }
    } else {
        const char *prev = cap->start;
        for (match_t *child = cap->child; child; child = child->nextsibling) {
            if (child->start > prev) {
                size_t len = (size_t)(child->start - prev);
                if ((flags & BP_IGNORECASE) ? memicmp(str, prev, len) != 0
                                            : memcmp(str, prev, len) != 0) {
                    return NULL;
                }
                str += len;
                prev = child->start;
            }
            if (child->start < prev) continue;
            str = match_backref(str, op, child, flags);
            if (str == NULL) return NULL;
            prev = child->end;
        }
        if (cap->end > prev) {
            size_t len = (size_t)(cap->end - prev);
            if ((flags & BP_IGNORECASE) ? memicmp(str, prev, len) != 0
                                        : memcmp(str, prev, len) != 0) {
                return NULL;
            }
            str += len;
        }
    }
    return str;
}


//
// Run virtual machine operation against a string and return
// a match struct, or NULL if no match is found.
// The returned value should be free()'d to avoid memory leaking.
//
static match_t *_match(def_t *defs, file_t *f, const char *str, vm_op_t *op, unsigned int flags)
{
    switch (op->type) {
        case VM_CANNED: {
            if (str == op->args.canned.at) {
                ++op->args.canned.visits;
                // TODO: deep copy?
                return op->args.canned.match;
            } else {
                return _match(defs, f, str, op->args.canned.fallback, flags);
            }
        }
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
            match_t *m = _match(defs, f, str, op->args.pat, flags);
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

            vm_op_t *pat = op->args.multiple.first, *skip = op->args.multiple.second;
            if (!pat && !skip) {
                while (str < f->end && *str != '\n') ++str;
                m->end = str;
                return m;
            }

            match_t **dest = &m->child;
            for (const char *prev = NULL; prev < str; ) {
                prev = str;
                if (pat) {
                    match_t *p = _match(defs, f, str, pat, flags);
                    if (p != NULL) {
                        set_owner(p, dest);
                        m->end = p->end;
                        return m;
                    }
                } else if (str == f->end) {
                    m->end = str;
                    return m;
                }
                if (skip) {
                    match_t *s = _match(defs, f, str, skip, flags);
                    if (s != NULL) {
                        set_owner(s, dest);
                        dest = &s->nextsibling;
                        str = s->end;
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
                    sep = _match(defs, f, str, op->args.repetitions.sep, flags);
                    if (sep == NULL) break;
                    str = sep->end;
                }
                match_t *p = _match(defs, f, str, op->args.repetitions.repeat_pat, flags);
                if (p == NULL) {
                    str = start;
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
                    set_owner(sep, dest);
                    dest = &sep->nextsibling;
                }
                set_owner(p, dest);
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
            match_t *before = _match(defs, f, str - backtrack, op->args.pat, flags);
            if (before == NULL) return NULL;
            match_t *m = new(match_t);
            m->start = str;
            m->end = str;
            m->op = op;
            set_owner(before, &m->child);
            return m;
        }
        case VM_BEFORE: {
            match_t *after = _match(defs, f, str, op->args.pat, flags);
            if (after == NULL) return NULL;
            match_t *m = new(match_t);
            m->start = str;
            m->end = str;
            m->op = op;
            set_owner(after, &m->child);
            return m;
        }
        case VM_CAPTURE: {
            match_t *p = _match(defs, f, str, op->args.pat, flags);
            if (p == NULL) return NULL;
            match_t *m = new(match_t);
            m->start = str;
            m->end = p->end;
            m->op = op;
            set_owner(p, &m->child);
            return m;
        }
        case VM_HIDE: {
            match_t *p = _match(defs, f, str, op->args.pat, flags);
            if (p == NULL) return NULL;
            match_t *m = new(match_t);
            m->start = str;
            m->end = p->end;
            m->op = op;
            set_owner(p, &m->child);
            return m;
        }
        case VM_OTHERWISE: {
            match_t *m = _match(defs, f, str, op->args.multiple.first, flags);
            if (m == NULL) m = _match(defs, f, str, op->args.multiple.second, flags);
            return m;
        }
        case VM_CHAIN: {
            match_t *m1 = _match(defs, f, str, op->args.multiple.first, flags);
            if (m1 == NULL) return NULL;

            match_t *m2;
            { // Push backrefs and run matching, then cleanup
                def_t *defs2 = with_backrefs(defs, f, m1);
                m2 = _match(defs2, f, m1->end, op->args.multiple.second, flags);
                free_defs(&defs2, defs);
            }

            if (m2 == NULL) {
                destroy_match(&m1);
                return NULL;
            }
            match_t *m = new(match_t);
            m->start = str;
            m->end = m2->end;
            m->op = op;
            set_owner(m1, &m->child);
            set_owner(m2, &m1->nextsibling);
            return m;
        }
        case VM_EQUAL: case VM_NOT_EQUAL: {
            match_t *m1 = _match(defs, f, str, op->args.multiple.first, flags);
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
            match_t *m2 = _match(defs, &inner, str, op->args.multiple.second, flags);
            if ((m2 == NULL) == (op->type == VM_EQUAL)) {
                destroy_match(&m1);
                if (m2 != NULL) destroy_match(&m2);
                return NULL;
            }
            match_t *m = new(match_t);
            m->start = m1->start;
            m->end = m1->end;
            m->op = op;
            set_owner(m1, &m->child);
            if (op->type == VM_EQUAL) {
                set_owner(m2, &m1->nextsibling);
            } else {
                destroy_match(&m2);
            }
            return m;
        }
        case VM_REPLACE: {
            match_t *p = NULL;
            if (op->args.replace.pat) {
                p = _match(defs, f, str, op->args.replace.pat, flags);
                if (p == NULL) return NULL;
            }
            match_t *m = new(match_t);
            m->start = str;
            m->op = op;
            if (p) {
                set_owner(p, &m->child);
                m->end = p->end;
            } else {
                m->end = m->start;
            }
            return m;
        }
        case VM_REF: {
            def_t *def = lookup(defs, op->args.s);
            check(def != NULL, "Unknown identifier: '%s'", op->args.s);
            vm_op_t *ref = def->op;

            vm_op_t fail_op = {
                .type = VM_CANNED,
                .start = ref->start,
                .end = ref->end,
                .len = 0,
                .args.canned = {
                    .match = NULL,
                    .visits = 0,
                    .at = str,
                    .fallback = ref,
                },
            };
            def_t defs2 = {
                .namelen = def->namelen,
                .name = def->name,
                .file = def->file,
                .op = &fail_op,
                .next = defs,
            };

            const char *prev = str;
            match_t *m = _match(&defs2, f, str, ref, flags);
            if (m == NULL) return NULL;

            while (fail_op.args.canned.visits > 0 && m->end > prev) {
                fail_op.args.canned.visits = 0;
                fail_op.args.canned.match = m;
                prev = m->end;
                match_t *m2 = _match(&defs2, f, str, ref, flags);
                if (m2 == NULL) break;
                m = m2;
            }

            return m;
        }
        case VM_BACKREF: {
            const char *end = match_backref(str, op, op->args.backref, flags);
            if (end == NULL) return NULL;
            match_t *m = new(match_t);
            m->op = op;
            m->start = str;
            m->end = end;
            return m;
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
            fprintf(stderr, "Unknown opcode: %d", op->type);
            _exit(1);
            return NULL;
        }
    }
}

//
// Get a specific numbered pattern capture.
//
static match_t *get_capture_by_num(match_t *m, int *n)
{
    if (*n == 0) return m;
    if (m->op->type == VM_CAPTURE && *n == 1) return m;
    if (m->op->type == VM_CAPTURE) --(*n);
    for (match_t *c = m->child; c; c = c->nextsibling) {
        match_t *cap = get_capture_by_num(c, n);
        if (cap) return cap;
    }
    return NULL;
}

//
// Get a capture with a specific name.
//
static match_t *get_capture_by_name(match_t *m, const char *name)
{
    if (m->op->type == VM_CAPTURE && m->op->args.capture.name
        && streq(m->op->args.capture.name, name))
        return m;
    for (match_t *c = m->child; c; c = c->nextsibling) {
        match_t *cap = get_capture_by_name(c, name);
        if (cap) return cap;
    }
    return NULL;
}

//
// Get a capture by identifier (name or number).
// Update *id to point to after the identifier (if found).
//
match_t *get_capture(match_t *m, const char **id)
{
    if (isdigit(**id)) {
        int n = (int)strtol(*id, (char**)id, 10);
        return get_capture_by_num(m->child, &n);
    } else {
        const char *end = after_name(*id);
        if (end == *id) return NULL;
        char *name = strndup(*id, (size_t)(end-*id));
        match_t *cap = get_capture_by_name(m, name);
        xfree(&name);
        *id = end;
        if (**id == ';') ++(*id);
        return cap;
    }
    return NULL;
}

//
// Wrapper function for _match() to kickstart the recursion info.
//
match_t *match(def_t *defs, file_t *f, const char *str, vm_op_t *op, unsigned int flags)
{
    return _match(defs, f, str, op, flags);
}

// Deallocate memory associated with an op
void destroy_op(vm_op_t *op)
{
    switch (op->type) {
        case VM_STRING: case VM_REF:
            xfree(&op->args.s);
            break;
        case VM_CAPTURE:
            if (op->args.capture.name)
                xfree(&op->args.capture.name);
            break;
        case VM_REPLACE:
            if (op->args.replace.text)
                xfree(&op->args.replace.text);
            break;
        default: break;
    }
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
