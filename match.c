//
// match.c - Code for the BP virtual machine that performs the matching.
//

#include <ctype.h>
#include <err.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "definitions.h"
#include "match.h"
#include "pattern.h"
#include "types.h"
#include "utils.h"
#include "utf8.h"

#ifdef DEBUG_HEAP
// Doubly-linked list operations:
#define DLL_PREPEND(head, node) do { (node)->home = &(head); (node)->next = head; if (head) (head)->home = &(node)->next; head = node; } while(false)
#define DLL_REMOVE(node) do { *(node)->home = (node)->next; if ((node)->next) (node)->next->home = (node)->home; } while(false)
#endif

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

typedef struct {
    size_t size, occupancy;
    match_t **matches;
} cache_t;

#define MAX_CACHE_SIZE (1<<14)

cache_t cache = {0, 0, NULL};

__attribute__((nonnull(1)))
static inline pat_t *deref(def_t *defs, pat_t *pat);
__attribute__((returns_nonnull))
static match_t *new_match(def_t *defs, pat_t *pat, const char *start, const char *end, match_t *children[]);
__attribute__((nonnull))
static match_t *get_capture_by_num(match_t *m, int *n);
__attribute__((nonnull, pure))
static match_t *get_capture_by_name(match_t *m, const char *name);
__attribute__((hot, nonnull(2,3,4)))
static match_t *match(def_t *defs, file_t *f, const char *str, pat_t *pat, bool ignorecase);

// Store a value and update its refcount
static inline void add_owner(match_t** owner, match_t* owned)
{
    if (*owner != NULL)
        errx(EXIT_FAILURE, "Ownership is being overwritten");
    *owner = owned;
    ++owned->refcount;
}

// Unstore a value and update its refcount
static inline void remove_ownership(match_t** owner)
{
    if (*owner) {
        --(*owner)->refcount;
        if ((*owner)->refcount == 0)
            recycle_if_unused(owner);
        *owner = NULL;
    }
}

// Helper method for concisely allocating children matches
// static match_t** _alloc_children(size_t n, match_t* matches[])
// {
//     if (n == 0) return NULL;
//     match_t **ret = xcalloc(n+1, sizeof(match_t*));
//     for (size_t i = 0; i < n; i++)
//         add_owner(&ret[i], matches[i]);
//     return ret;
// }

// #define MATCHES(...) _alloc_children((sizeof((match_t*[]){__VA_ARGS__}))/sizeof(match_t*), (match_t*[]){__VA_ARGS__})

#define MATCHES(...) (match_t*[]){__VA_ARGS__, NULL}

static inline size_t hash(const char *str, pat_t *pat)
{
    return (size_t)str + 2*pat->id;
}

static match_t *cache_lookup(def_t *defs, const char *str, pat_t *pat)
{
    if (!cache.matches) return NULL;
    size_t h = hash(str, pat) & (cache.size-1);
    for (match_t *c = cache.matches[h]; c; c = c->cache_next) {
        if (c->pat == pat && c->defs_id == defs->id && c->start == str)
            return c;
    }
    return NULL;
}

static void cache_remove(match_t *m)
{
    if (!m->cache_home) return;
    *m->cache_home = m->cache_next;
    if (m->cache_next) m->cache_next->cache_home = m->cache_home;
    m->cache_next = NULL;
    m->cache_home = NULL;
    remove_ownership(&m);
    --cache.occupancy;
}

static void cache_save(match_t *m)
{
    if (cache.occupancy+1 > 3*cache.size) {
        if (cache.size == MAX_CACHE_SIZE) {
            size_t h = hash(m->start, m->pat) & (cache.size-1);
            for (int quota = 2; cache.matches[h] && quota > 0; quota--) {
                match_t *last = cache.matches[h];
                while (last->cache_next) last = last->cache_next;
                cache_remove(last);
            }
        } else {
            cache_t old_cache = cache;
            cache.size = old_cache.size == 0 ? 16 : 2*old_cache.size;
            cache.matches = xcalloc(cache.size, sizeof(match_t*));

            // Rehash:
            if (old_cache.matches) {
                for (size_t i = 0; i < old_cache.size; i++) {
                    for (match_t *o; (o = old_cache.matches[i]); ) {
                        *o->cache_home = o->cache_next;
                        if (o->cache_next) o->cache_next->cache_home = o->cache_home;
                        size_t h = hash(o->start, o->pat) & (cache.size-1);
                        o->cache_home = &(cache.matches[h]);
                        o->cache_next = cache.matches[h];
                        if (cache.matches[h]) cache.matches[h]->cache_home = &o->cache_next;
                        cache.matches[h] = o;
                    }
                }
                free(old_cache.matches);
            }
        }
    }

    size_t h = hash(m->start, m->pat) & (cache.size-1);
    m->cache_home = &(cache.matches[h]);
    m->cache_next = cache.matches[h];
    if (cache.matches[h]) cache.matches[h]->cache_home = &m->cache_next;
    cache.matches[h] = NULL;
    add_owner(&cache.matches[h], m);
    ++cache.occupancy;
}

static void cache_prune(const char *start, const char *end)
{
    if (!cache.matches) return;
    for (size_t i = 0; i < cache.size; i++) {
        for (match_t *m = cache.matches[i], *next = NULL; m; m = next) {
            next = m->cache_next;
            if (m->start < start || (m->end ? m->end : m->start) > end)
                cache_remove(m);
        }
    }
}

void cache_destroy(void)
{
    if (!cache.matches) return;
    for (size_t i = 0; i < cache.size; i++) {
        while (cache.matches[i])
            cache_remove(cache.matches[i]);
    }
    cache.occupancy = 0;
    xfree(&cache.matches);
    cache.size = 0;
}

//
// If the given pattern is a reference, look it up and return the referenced
// pattern. This is used for an optimization to avoid repeated lookups.
//
static inline pat_t *deref(def_t *defs, pat_t *pat)
{
    if (pat && pat->type == BP_REF) {
        def_t *def = lookup(defs, pat->args.ref.len, pat->args.ref.name);
        if (def) pat = def->pat;
    }
    return pat;
}

//
// Find and return the first and simplest pattern that will definitely have to
// match for the whole pattern to match (if any)
//
static pat_t *first_pat(def_t *defs, pat_t *pat)
{
    for (pat_t *p = pat; p; ) {
        switch (p->type) {
            case BP_BEFORE:
                p = p->args.pat; break;
            case BP_REPEAT:
                if (p->args.repetitions.min == 0)
                    return p;
                p = p->args.repetitions.repeat_pat; break;
            case BP_CAPTURE:
                p = p->args.capture.capture_pat; break;
            case BP_CHAIN: case BP_MATCH: case BP_NOT_MATCH:
                p = p->args.multiple.first; break;
            case BP_REPLACE:
                p = p->args.replace.pat; break;
            case BP_REF: {
                pat_t *p2 = deref(defs, p);
                if (p2 == p) return p2;
                p = p2;
                break;
            }
            default: return p;
        }
    }
    return pat;
}

//
// Find the next match after prev (or the first match if prev is NULL)
//
match_t *next_match(def_t *defs, file_t *f, match_t *prev, pat_t *pat, pat_t *skip, bool ignorecase)
{
    const char *str;
    if (prev) {
        str = prev->end > prev->start ? prev->end : prev->end + 1;
        if (prev->refcount == 0) recycle_if_unused(&prev);
        cache_prune(str, f->end);
    } else {
        str = f->start;
    }

    pat = deref(defs, pat);
    pat_t *first = first_pat(defs, pat);

    // Performance optimization: if the pattern starts with a string literal,
    // we can just rely on the highly optimized strstr()/strcasestr()
    // implementations to skip past areas where we know we won't find a match.
    if (!skip && first->type == BP_STRING) {
        for (size_t i = 0; i < first->min_matchlen; i++)
            if (first->args.string[i] == '\0')
                goto pattern_search;
        char *tmp = strndup(first->args.string, first->min_matchlen);
        char *found = (ignorecase ? strcasestr : strstr)(str, tmp);
        if (found)
            str = found;
        else
            str += strlen(str); // Use += strlen here instead of f->end to handle files with NULL bytes
        free(tmp);
    }

  pattern_search:
    if (str > f->end) return NULL;

    do {
        match_t *m = match(defs, f, str, pat, ignorecase);
        if (m) return m;
        if (first->type == BP_START_OF_FILE) return NULL;
        match_t *s;
        if (skip && (s = match(defs, f, str, skip, ignorecase))) {
            str = s->end > str ? s->end : str + 1;
            recycle_if_unused(&s);
        } else str = next_char(f, str);
    } while (str < f->end);
    return NULL;
}

//
// Attempt to match the given pattern against the input string and return a
// match object, or NULL if no match is found.
// The returned value should be free()'d to avoid memory leaking.
//
static match_t *match(def_t *defs, file_t *f, const char *str, pat_t *pat, bool ignorecase)
{
    switch (pat->type) {
        case BP_LEFTRECURSION: {
            // Left recursion occurs when a pattern directly or indirectly
            // invokes itself at the same position in the text. It's handled as
            // a special case, but if a pattern invokes itself at a later
            // point, it can be handled with normal recursion.
            // See: left-recursion.md for more details.
            if (str == pat->args.leftrec.at) {
                ++pat->args.leftrec.visits;
                return pat->args.leftrec.match;
            } else {
                return match(defs, f, str, pat->args.leftrec.fallback, ignorecase);
            }
        }
        case BP_ANYCHAR: {
            return (str < f->end && *str != '\n') ? new_match(defs, pat, str, next_char(f, str), NULL) : NULL;
        }
        case BP_ID_START: {
            return (str < f->end && isidstart(f, str)) ? new_match(defs, pat, str, next_char(f, str), NULL) : NULL;
        }
        case BP_ID_CONTINUE: {
            return (str < f->end && isidcontinue(f, str)) ? new_match(defs, pat, str, next_char(f, str), NULL) : NULL;
        }
        case BP_START_OF_FILE: {
            return (str == f->start) ? new_match(defs, pat, str, str, NULL) : NULL;
        }
        case BP_START_OF_LINE: {
            return (str == f->start || str[-1] == '\n') ? new_match(defs, pat, str, str, NULL) : NULL;
        }
        case BP_END_OF_FILE: {
            return (str == f->end) ? new_match(defs, pat, str, str, NULL) : NULL;
        }
        case BP_END_OF_LINE: {
            return (str == f->end || *str == '\n') ? new_match(defs, pat, str, str, NULL) : NULL;
        }
        case BP_WORD_BOUNDARY: {
            return (isidcontinue(f, str) != isidcontinue(f, prev_char(f, str))) ? new_match(defs, pat, str, str, NULL) : NULL;
        }
        case BP_STRING: {
            if (&str[pat->min_matchlen] > f->end) return NULL;
            if (pat->min_matchlen > 0 && (ignorecase ? memicmp : memcmp)(str, pat->args.string, pat->min_matchlen) != 0)
                return NULL;
            return new_match(defs, pat, str, str + pat->min_matchlen, NULL);
        }
        case BP_RANGE: {
            if (str >= f->end) return NULL;
            if ((unsigned char)*str < pat->args.range.low || (unsigned char)*str > pat->args.range.high)
                return NULL;
            return new_match(defs, pat, str, str+1, NULL);
        }
        case BP_NOT: {
            match_t *m = match(defs, f, str, pat->args.pat, ignorecase);
            if (m != NULL) {
                recycle_if_unused(&m);
                return NULL;
            }
            return new_match(defs, pat, str, str, NULL);
        }
        case BP_UPTO: {
            match_t *m = new_match(defs, pat, str, str, NULL);
            pat_t *target = deref(defs, pat->args.multiple.first),
                  *skip = deref(defs, pat->args.multiple.second);
            if (!target && !skip) {
                while (str < f->end && *str != '\n') ++str;
                m->end = str;
                return m;
            }

            size_t child_cap = 0, nchildren = 0;
            for (const char *prev = NULL; prev < str; ) {
                prev = str;
                if (target) {
                    match_t *p = match(defs, f, str, target, ignorecase);
                    if (p != NULL) {
                        recycle_if_unused(&p);
                        m->end = str;
                        return m;
                    }
                } else if (str == f->end) {
                    m->end = str;
                    return m;
                }
                if (skip) {
                    match_t *s = match(defs, f, str, skip, ignorecase);
                    if (s != NULL) {
                        str = s->end;
                        if (nchildren+2 >= child_cap) {
                            m->children = xrealloc(m->children, sizeof(match_t*)*(child_cap += 5));
                            for (size_t i = nchildren; i < child_cap; i++) m->children[i] = NULL;
                        }
                        add_owner(&m->children[nchildren++], s);
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
        case BP_REPEAT: {
            match_t *m = new_match(defs, pat, str, str, NULL);
            size_t reps = 0;
            ssize_t max = pat->args.repetitions.max;
            pat_t *repeating = deref(defs, pat->args.repetitions.repeat_pat);
            pat_t *sep = deref(defs, pat->args.repetitions.sep);
            size_t child_cap = 0, nchildren = 0;
            for (reps = 0; max == -1 || reps < (size_t)max; ++reps) {
                const char *start = str;
                // Separator
                match_t *msep = NULL;
                if (sep != NULL && reps > 0) {
                    msep = match(defs, f, str, sep, ignorecase);
                    if (msep == NULL) break;
                    str = msep->end;
                }
                match_t *mp = match(defs, f, str, repeating, ignorecase);
                if (mp == NULL) {
                    str = start;
                    if (msep) recycle_if_unused(&msep);
                    break;
                }
                if (mp->end == start && reps > 0) {
                    // Since no forward progress was made on either `repeating`
                    // or `sep` and BP does not have mutable state, it's
                    // guaranteed that no progress will be made on the next
                    // loop either. We know that this will continue to loop
                    // until reps==max, so let's just cut to the chase instead
                    // of looping infinitely.
                    if (msep) recycle_if_unused(&msep);
                    recycle_if_unused(&mp);
                    if (pat->args.repetitions.max == -1)
                        reps = ~(size_t)0;
                    else
                        reps = (size_t)pat->args.repetitions.max;
                    break;
                }
                if (msep) {
                    if (nchildren+2 >= child_cap) {
                        m->children = xrealloc(m->children, sizeof(match_t*)*(child_cap += 5));
                        for (size_t i = nchildren; i < child_cap; i++) m->children[i] = NULL;
                    }
                    add_owner(&m->children[nchildren++], msep);
                }

                if (nchildren+2 >= child_cap) {
                    m->children = xrealloc(m->children, sizeof(match_t*)*(child_cap += 5));
                    for (size_t i = nchildren; i < child_cap; i++) m->children[i] = NULL;
                }
                add_owner(&m->children[nchildren++], mp);
                str = mp->end;
            }

            if (reps < (size_t)pat->args.repetitions.min) {
                recycle_if_unused(&m);
                return NULL;
            }
            m->end = str;
            return m;
        }
        case BP_AFTER: {
            pat_t *back = deref(defs, pat->args.pat);
            if (!back) return NULL;

            // We only care about the region from the backtrack pos up to the
            // current pos, so mock it out as a file slice.
            // TODO: this breaks ^/^^/$/$$, but that can probably be ignored
            // because you rarely need to check those in a backtrack.
            file_t slice;
            slice_file(&slice, f, f->start, str);
            for (const char *pos = &str[-(long)back->min_matchlen];
              pos >= f->start && (back->max_matchlen == -1 || pos >= &str[-(long)back->max_matchlen]);
              pos = prev_char(f, pos)) {
                slice.start = (char*)pos;
                match_t *m = match(defs, &slice, pos, back, ignorecase);
                // Match should not go past str (i.e. (<"AB" "B") should match "ABB", but not "AB")
                if (m && m->end != str)
                    recycle_if_unused(&m);
                else if (m)
                    return new_match(defs, pat, str, str, MATCHES(m));
                if (pos == f->start) break;
                // To prevent extreme performance degradation, don't keep
                // walking backwards endlessly over newlines.
                if (back->max_matchlen == -1 && *pos == '\n') break;
            }
            return NULL;
        }
        case BP_BEFORE: {
            match_t *after = match(defs, f, str, pat->args.pat, ignorecase);
            return after ? new_match(defs, pat, str, str, MATCHES(after)) : NULL;
        }
        case BP_CAPTURE: {
            match_t *p = match(defs, f, str, pat->args.pat, ignorecase);
            return p ? new_match(defs, pat, str, p->end, MATCHES(p)) : NULL;
        }
        case BP_OTHERWISE: {
            match_t *m = match(defs, f, str, pat->args.multiple.first, ignorecase);
            return m ? m : match(defs, f, str, pat->args.multiple.second, ignorecase);
        }
        case BP_CHAIN: {
            match_t *m1 = match(defs, f, str, pat->args.multiple.first, ignorecase);
            if (m1 == NULL) return NULL;

            match_t *m2;
            // Push backrefs and run matching, then cleanup
            if (m1->pat->type == BP_CAPTURE && m1->pat->args.capture.name) {
                // Temporarily add a rule that the backref name matches the
                // exact string of the original match (no replacements)
                size_t len = (size_t)(m1->end - m1->start);
                pat_t *backref = new_pat(f, m1->start, m1->end, len, (ssize_t)len, BP_STRING);
                backref->args.string = m1->start;

                def_t *defs2 = with_def(defs, m1->pat->args.capture.namelen, m1->pat->args.capture.name, backref);
                ++m1->refcount; {
                    m2 = match(defs2, f, m1->end, pat->args.multiple.second, ignorecase);
                    if (!m2) { // No need to keep the backref in memory if it didn't match
                        for (pat_t **rem = &f->pats; *rem; rem = &(*rem)->next) {
                            if ((*rem) == backref) {
                                pat_t *tmp = *rem;
                                *rem = (*rem)->next;
                                free(tmp);
                                break;
                            }
                        }
                    }
                    defs = free_defs(defs2, defs);
                } --m1->refcount;
            } else {
                m2 = match(defs, f, m1->end, pat->args.multiple.second, ignorecase);
            }

            if (m2 == NULL) {
                recycle_if_unused(&m1);
                return NULL;
            }

            return new_match(defs, pat, str, m2->end, MATCHES(m1, m2));
        }
        case BP_MATCH: case BP_NOT_MATCH: {
            match_t *m1 = match(defs, f, str, pat->args.multiple.first, ignorecase);
            if (m1 == NULL) return NULL;

            // <p1>~<p2> matches iff the text of <p1> matches <p2>
            // <p1>!~<p2> matches iff the text of <p1> does not match <p2>
            file_t slice;
            slice_file(&slice, f, m1->start, m1->end);
            match_t *m2 = next_match(defs, &slice, NULL, pat->args.multiple.second, NULL, ignorecase);
            if ((!m2 && pat->type == BP_MATCH) || (m2 && pat->type == BP_NOT_MATCH)) {
                if (m2) recycle_if_unused(&m2);
                recycle_if_unused(&m1);
                return NULL;
            }
            return new_match(defs, pat, m1->start, m1->end, (pat->type == BP_MATCH) ? MATCHES(m2) : NULL);
        }
        case BP_REPLACE: {
            match_t *p = NULL;
            if (pat->args.replace.pat) {
                p = match(defs, f, str, pat->args.replace.pat, ignorecase);
                if (p == NULL) return NULL;
            }
            return new_match(defs, pat, str, p ? p->end : str, MATCHES(p));
        }
        case BP_REF: {
            match_t *cached = cache_lookup(defs, str, pat);
            if (cached) return cached->end == NULL ? NULL : cached;

            def_t *def = lookup(defs, pat->args.ref.len, pat->args.ref.name);
            if (def == NULL)
                errx(EXIT_FAILURE, "Unknown identifier: '%.*s'", (int)pat->args.ref.len, pat->args.ref.name);
            pat_t *ref = def->pat;

            pat_t rec_op = {
                .type = BP_LEFTRECURSION,
                .start = ref->start,
                .end = ref->end,
                .min_matchlen = 0,
                .max_matchlen = -1,
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
                .pat = &rec_op,
                .next = defs,
            };

            const char *prev = str;
            match_t *m = match(&defs2, f, str, ref, ignorecase);
            if (m == NULL) {
                // Store placeholder:
                cache_save(new_match(defs, pat, str, NULL, NULL));
                return NULL;
            }

            while (rec_op.args.leftrec.visits > 0) {
                rec_op.args.leftrec.visits = 0;
                remove_ownership(&rec_op.args.leftrec.match);
                add_owner(&rec_op.args.leftrec.match, m);
                prev = m->end;
                match_t *m2 = match(&defs2, f, str, ref, ignorecase);
                if (m2 == NULL) break;
                if (m2->end <= prev) {
                    recycle_if_unused(&m2);
                    break;
                }
                m = m2;
            }

            // This match wrapper mainly exists for record-keeping purposes.
            // However, it also keeps `m` from getting garbage collected with
            // leftrec.match is GC'd. It also helps with visualization of match
            // results.
            // OPTIMIZE: remove this if necessary
            match_t *wrap = new_match(defs, pat, m->start, m->end, MATCHES(m));
            cache_save(wrap);

            if (rec_op.args.leftrec.match)
                remove_ownership(&rec_op.args.leftrec.match);

            return wrap;
        }
        case BP_NODENT: {
            if (*str != '\n') return NULL;
            const char *start = str;

            size_t linenum = get_line_number(f, str);
            const char *p = get_line(f, linenum);
            if (p < f->start) p = f->start; // Can happen with recursive matching

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

            return new_match(defs, pat, start, &str[dents], NULL);
        }
        case BP_ERROR: {
            match_t *p = pat->args.pat ? match(defs, f, str, pat->args.pat, ignorecase) : NULL;
            return p ? new_match(defs, pat, str, p->end, MATCHES(p)) : NULL;
        }
        default: {
            errx(EXIT_FAILURE, "Unknown pattern type: %u", pat->type);
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
    if (m->pat->type == BP_CAPTURE && *n == 1) return m;
    if (m->pat->type == BP_CAPTURE) --(*n);
    if (m->children) {
        for (int i = 0; m->children[i]; i++) {
            match_t *cap = get_capture_by_num(m->children[i], n);
            if (cap) return cap;
        }
    }
    return NULL;
}

//
// Get a capture with a specific name.
//
static match_t *get_capture_by_name(match_t *m, const char *name)
{
    if (m->pat->type == BP_CAPTURE && m->pat->args.capture.name
        && strncmp(m->pat->args.capture.name, name, m->pat->args.capture.namelen) == 0)
        return m;
    if (m->children) {
        for (int i = 0; m->children[i]; i++) {
            match_t *cap = get_capture_by_name(m->children[i], name);
            if (cap) return cap;
        }
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
        return get_capture_by_num(m, &n);
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
}

//
// Return a match object which can be used (may be allocated or recycled).
//
static match_t *new_match(def_t *defs, pat_t *pat, const char *start, const char *end, match_t *children[])
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
        (void)memset(m, 0, sizeof(match_t));
    } else {
        m = new(match_t);
    }
#endif

    m->pat = pat;
    m->start = start;
    m->end = end;
    m->defs_id = defs->id;

    if (children) {
        for (int i = 0; children[i]; i++)
            add_owner(&m->_children[i], children[i]);
        m->children = m->_children;
    }
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

    if (m->children) {
        for (int i = 0; m->children[i]; i++)
            remove_ownership(&m->children[i]);
        if (m->children != m->_children)
            xfree(&m->children);
    }

#ifdef DEBUG_HEAP
    DLL_REMOVE(m); // Remove from in_use_matches
    (void)memset(m, 0, sizeof(match_t));
    DLL_PREPEND(unused_matches, m);
#else
    (void)memset(m, 0, sizeof(match_t));
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
        if (m->children && m->children != m->_children)
            xfree(&m->children);
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

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
