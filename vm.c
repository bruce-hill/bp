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

#ifdef DEBUG_HEAP
// Doubly-linked list operations:
#define DLL_PREPEND(head, node) do { (node)->atme = &(head); (node)->next = head; if (head) (head)->atme = &(node)->next; head = node; } while(0)
#define DLL_REMOVE(node) do { *(node)->atme = (node)->next; if ((node)->next) (node)->next->atme = (node)->atme; } while(0)
#endif

// Refcounting ownership-setting macros:
#define ADD_OWNER(owner, m) do { owner = m; ++(m)->refcount; } while(0)
#define REMOVE_OWNERSHIP(owner) do { if (owner) { --(owner)->refcount; recycle_if_unused(&(owner)); owner = NULL; } } while(0)

// New match objects are either recycled from unused match objects or allocated
// from the heap. While it is in use, the match object is stored in the
// `in_use_matches` linked list. Once it is no longer needed, it is moved to
// the `unused_matches` linked list so it can be reused without the need for
// additional calls to malloc/free. Thus, it is an invariant that every match
// object is in one of these two lists:
static match_t *unused_matches = NULL;
#ifdef DEBUG_HEAP
static match_t *in_use_matches = NULL;
#endif

__attribute__((nonnull, pure))
static inline const char *next_char(file_t *f, const char *str);
__attribute__((nonnull))
static const char *match_backref(const char *str, vm_op_t *op, match_t *cap, unsigned int flags);
__attribute__((nonnull))
static match_t *get_capture_by_num(match_t *m, int *n);
__attribute__((nonnull, pure))
static match_t *get_capture_by_name(match_t *m, const char *name);

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
match_t *match(def_t *defs, file_t *f, const char *str, vm_op_t *op, unsigned int flags)
{
    switch (op->type) {
        case VM_LEFTRECURSION: {
            // Left recursion occurs when a pattern directly or indirectly
            // invokes itself at the same position in the text. It's handled as
            // a special case, but if a pattern invokes itself at a later
            // point, it can be handled with normal recursion.
            // See: left-recursion.md for more details.
            if (str == op->args.leftrec.at) {
                ++op->args.leftrec.visits;
                return op->args.leftrec.match;
            } else {
                return match(defs, f, str, op->args.leftrec.fallback, flags);
            }
        }
        case VM_ANYCHAR: {
            if (str >= f->end || *str == '\n')
                return NULL;
            match_t *m = new_match();
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
            match_t *m = new_match();
            m->op = op;
            m->start = str;
            m->end = str + op->len;
            return m;
        }
        case VM_RANGE: {
            if (str >= f->end) return NULL;
            if ((unsigned char)*str < op->args.range.low || (unsigned char)*str > op->args.range.high)
                return NULL;
            match_t *m = new_match();
            m->op = op;
            m->start = str;
            m->end = str + 1;
            return m;
        }
        case VM_NOT: {
            match_t *m = match(defs, f, str, op->args.pat, flags);
            if (m != NULL) {
                recycle_if_unused(&m);
                return NULL;
            }
            m = new_match();
            m->op = op;
            m->start = str;
            m->end = str;
            return m;
        }
        case VM_UPTO_AND: {
            match_t *m = new_match();
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
                    match_t *p = match(defs, f, str, pat, flags);
                    if (p != NULL) {
                        ADD_OWNER(*dest, p);
                        m->end = p->end;
                        return m;
                    }
                } else if (str == f->end) {
                    m->end = str;
                    return m;
                }
                if (skip) {
                    match_t *s = match(defs, f, str, skip, flags);
                    if (s != NULL) {
                        ADD_OWNER(*dest, s);
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
            recycle_if_unused(&m);
            return NULL;
        }
        case VM_REPEAT: {
            match_t *m = new_match();
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
                    sep = match(defs, f, str, op->args.repetitions.sep, flags);
                    if (sep == NULL) break;
                    str = sep->end;
                }
                match_t *p = match(defs, f, str, op->args.repetitions.repeat_pat, flags);
                if (p == NULL) {
                    str = start;
                    recycle_if_unused(&sep);
                    break;
                }
                if (p->end == start && reps > 0) {
                    // Since no forward progress was made on either `pat` or
                    // `sep` and BP does not have mutable state, it's
                    // guaranteed that no progress will be made on the next
                    // loop either. We know that this will continue to loop
                    // until reps==max, so let's just cut to the chase instead
                    // of looping infinitely.
                    recycle_if_unused(&sep);
                    recycle_if_unused(&p);
                    if (op->args.repetitions.max == -1)
                        reps = ~(size_t)0;
                    else
                        reps = (size_t)op->args.repetitions.max;
                    break;
                }
                if (sep) {
                    ADD_OWNER(*dest, sep);
                    dest = &sep->nextsibling;
                }
                ADD_OWNER(*dest, p);
                dest = &p->nextsibling;
                str = p->end;
            }

            if (reps < (size_t)op->args.repetitions.min) {
                recycle_if_unused(&m);
                return NULL;
            }
            m->end = str;
            return m;
        }
        case VM_AFTER: {
            ssize_t backtrack = op->args.pat->len;
            check(backtrack != -1, "'<' is only allowed for fixed-length operations");
            if (str - backtrack < f->contents) return NULL;
            match_t *before = match(defs, f, str - backtrack, op->args.pat, flags);
            if (before == NULL) return NULL;
            match_t *m = new_match();
            m->start = str;
            m->end = str;
            m->op = op;
            ADD_OWNER(m->child, before);
            return m;
        }
        case VM_BEFORE: {
            match_t *after = match(defs, f, str, op->args.pat, flags);
            if (after == NULL) return NULL;
            match_t *m = new_match();
            m->start = str;
            m->end = str;
            m->op = op;
            ADD_OWNER(m->child, after);
            return m;
        }
        case VM_CAPTURE: {
            match_t *p = match(defs, f, str, op->args.pat, flags);
            if (p == NULL) return NULL;
            match_t *m = new_match();
            m->start = str;
            m->end = p->end;
            m->op = op;
            ADD_OWNER(m->child, p);
            return m;
        }
        case VM_HIDE: {
            match_t *p = match(defs, f, str, op->args.pat, flags);
            if (p == NULL) return NULL;
            match_t *m = new_match();
            m->start = str;
            m->end = p->end;
            m->op = op;
            ADD_OWNER(m->child, p);
            return m;
        }
        case VM_OTHERWISE: {
            match_t *m = match(defs, f, str, op->args.multiple.first, flags);
            if (m == NULL) m = match(defs, f, str, op->args.multiple.second, flags);
            return m;
        }
        case VM_CHAIN: {
            match_t *m1 = match(defs, f, str, op->args.multiple.first, flags);
            if (m1 == NULL) return NULL;

            match_t *m2;
            { // Push backrefs and run matching, then cleanup
                def_t *defs2 = with_backrefs(defs, f, m1);
                m2 = match(defs2, f, m1->end, op->args.multiple.second, flags);
                free_defs(&defs2, defs);
            }

            if (m2 == NULL) {
                recycle_if_unused(&m1);
                return NULL;
            }
            match_t *m = new_match();
            m->start = str;
            m->end = m2->end;
            m->op = op;
            ADD_OWNER(m->child, m1);
            ADD_OWNER(m1->nextsibling, m2);
            return m;
        }
        case VM_EQUAL: case VM_NOT_EQUAL: {
            match_t *m1 = match(defs, f, str, op->args.multiple.first, flags);
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
            match_t *m2 = match(defs, &inner, str, op->args.multiple.second, flags);
            if ((m2 == NULL) == (op->type == VM_EQUAL)) {
                recycle_if_unused(&m1);
                if (m2 != NULL) recycle_if_unused(&m2);
                return NULL;
            }
            match_t *m = new_match();
            m->start = m1->start;
            m->end = m1->end;
            m->op = op;
            ADD_OWNER(m->child, m1);
            if (op->type == VM_EQUAL) {
                ADD_OWNER(m1->nextsibling, m2);
            } else {
                recycle_if_unused(&m2);
            }
            return m;
        }
        case VM_REPLACE: {
            match_t *p = NULL;
            if (op->args.replace.pat) {
                p = match(defs, f, str, op->args.replace.pat, flags);
                if (p == NULL) return NULL;
            }
            match_t *m = new_match();
            m->start = str;
            m->op = op;
            if (p) {
                ADD_OWNER(m->child, p);
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

            vm_op_t rec_op = {
                .type = VM_LEFTRECURSION,
                .start = ref->start,
                .end = ref->end,
                .len = 0,
                .args.leftrec = {
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
                .op = &rec_op,
                .next = defs,
            };

            const char *prev = str;
            match_t *m = match(&defs2, f, str, ref, flags);
            if (m == NULL) return NULL;

            while (rec_op.args.leftrec.visits > 0) {
                rec_op.args.leftrec.visits = 0;
                REMOVE_OWNERSHIP(rec_op.args.leftrec.match);
                ADD_OWNER(rec_op.args.leftrec.match, m);
                prev = m->end;
                match_t *m2 = match(&defs2, f, str, ref, flags);
                if (m2 == NULL) break;
                if (m2->end <= prev) {
                    recycle_if_unused(&m2);
                    break;
                }
                m = m2;
            }

            if (rec_op.args.leftrec.match) {
                // Ensure that `m` isn't garbage collected right now, but do
                // clean up the recursive match result if it's not needed.
                ++m->refcount;
                REMOVE_OWNERSHIP(rec_op.args.leftrec.match);
                --m->refcount;
            }

            return m;
        }
        case VM_BACKREF: {
            const char *end = match_backref(str, op, op->args.backref, flags);
            if (end == NULL) return NULL;
            match_t *m = new_match();
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

            match_t *m = new_match();
            m->start = start;
            m->end = &str[dents];
            m->op = op;
            return m;
        }
        default: {
            fprintf(stderr, "Unknown opcode: %d", op->type);
            exit(1);
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
// Return a match object which can be used (may be allocated or recycled).
//
match_t *new_match(void)
{
    match_t *m;

#ifdef DEBUG_HEAP
    if (unused_matches) {
        m = unused_matches;
        DLL_REMOVE(m);
        memset(m, 0, sizeof(match_t));
    } else {
        m = new(match_t);
    }
    // Keep track of the object:
    DLL_PREPEND(in_use_matches, m);
#else
    if (unused_matches) {
        m = unused_matches;
        unused_matches = unused_matches->next;
        memset(m, 0, sizeof(match_t));
    } else {
        m = new(match_t);
    }
#endif

    return m;
}

//
// If the given match is not currently a child member of another match (or
// otherwise reserved) then put it back in the pool of unused match objects.
//
void recycle_if_unused(match_t **at_m)
{
    match_t *m = *at_m;
    if (m == NULL) return;
    if (m->refcount > 0) {
        *at_m = NULL;
        return;
    }

    REMOVE_OWNERSHIP(m->child);
    REMOVE_OWNERSHIP(m->nextsibling);

#ifdef DEBUG_HEAP
    DLL_REMOVE(m); // Remove from in_use_matches
    memset(m, 0, sizeof(match_t));
    DLL_PREPEND(unused_matches, m);
#else
    memset(m, 0, sizeof(match_t));
    m->next = unused_matches;
    unused_matches = m;
#endif

    *at_m = NULL;
}

#ifdef DEBUG_HEAP
//
// Force all match objects into the pool of unused match objects.
//
size_t recycle_all_matches(void)
{
    size_t count = 0;
    while (in_use_matches) {
        match_t *m = in_use_matches;
        DLL_REMOVE(m);
        DLL_PREPEND(unused_matches, m);
        ++count;
    }
    return count;
}

//
// Free all match objects in memory.
//
size_t free_all_matches(void)
{
    size_t count = 0;
    recycle_all_matches();
    while (unused_matches) {
        match_t *m = unused_matches;
        DLL_REMOVE(m);
        free(m);
        ++count;
    }
    return count;
}
#endif

//
// Deallocate memory associated with an op
//
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
