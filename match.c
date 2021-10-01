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

#include "match.h"
#include "pattern.h"
#include "utils.h"
#include "utf8.h"

#define MAX_CACHE_SIZE (1<<14)

typedef struct {
    pat_t *pat;
    const char *start, *end;
} cache_hit_t;

// Cache datastructure
typedef struct {
    size_t size, occupancy;
    cache_hit_t *hits;
} cache_t;

// Data structure for various ambient state for matching
typedef struct match_ctx_s {
    struct match_ctx_s *parent_ctx;
    pat_t *defs;
    cache_t *cache;
    const char *start, *end;
    bool ignorecase;
} match_ctx_t;

// New match objects are either recycled from unused match objects or allocated
// from the heap. While it is in use, the match object is stored in the
// `in_use_matches` linked list. Once it is no longer needed, it is moved to
// the `unused_matches` linked list so it can be reused without the need for
// additional calls to malloc/free. Thus, it is an invariant that every match
// object is in one of these two lists:
static match_t *unused_matches = NULL;
static match_t *in_use_matches = NULL;

#define MATCHES(...) (match_t*[]){__VA_ARGS__, NULL}

__attribute__((hot, nonnull(1,2,3)))
static match_t *match(match_ctx_t *ctx, const char *str, pat_t *pat);
__attribute__((returns_nonnull))
static match_t *new_match(pat_t *pat, const char *start, const char *end, match_t *children[]);

// Prepend to a doubly linked list
static inline void gc_list_prepend(match_t **head, match_t *m)
{
    if (m->gc.home)
        errx(1, "Node already has a home");
    m->gc.home = head;
    m->gc.next = *head;
    if (*head) (*head)->gc.home = &m->gc.next;
    *head = m;
}

// Remove from a doubly linked list
static inline void gc_list_remove(match_t *m)
{
    if (!m->gc.home)
        errx(1, "Attempt to remove something that isn't in a list");
    *m->gc.home = m->gc.next;
    if (m->gc.next) m->gc.next->gc.home = m->gc.home;
    m->gc.home = NULL;
    m->gc.next = NULL;
}

//
// Hash a string position/pattern.
//
static inline size_t hash(const char *str, size_t pat_id)
{
    return (size_t)str + 2*pat_id;
}

//
// Check if we have memoized a pattern match at the given position for the
// given definitions. If a result has been memoized, set *result to the
// memoized value and return true, otherwise return false.
//
static cache_hit_t cache_get(match_ctx_t *ctx, const char *str, pat_t *pat)
{
    if (!ctx->cache->hits) return (cache_hit_t){0};
    size_t h = hash(str, pat->id) & (ctx->cache->size-1);
    cache_hit_t c = ctx->cache->hits[h];
    if (c.pat == pat && c.start == str)
        return c;
    return (cache_hit_t){0};
}

//
// Save a match in the cache.
//
static void cache_save(match_ctx_t *ctx, const char *str, pat_t *pat, match_t *m)
{
    cache_t *cache = ctx->cache;
    if (cache->occupancy+1 > (cache->size*1)/5) {
        cache_hit_t *old_hits = cache->hits;
        size_t old_size = cache->size;
        cache->size = old_size == 0 ? 16 : 2*old_size;
        cache->hits = new(cache_hit_t[cache->size]);

        // Rehash:
        for (size_t i = 0; i < old_size; i++) {
            if (old_hits[i].pat) {
                size_t h = hash(old_hits[i].start, old_hits[i].pat->id) & (cache->size-1);
                cache->hits[h] = old_hits[i];
            }
        }
        if (old_hits) delete(&old_hits);
    }

    size_t h = hash(str, pat->id) & (cache->size-1);
    if (!cache->hits[h].start) ++cache->occupancy;
    cache->hits[h].start = str;
    cache->hits[h].end = m ? m->end : NULL;
    cache->hits[h].pat = pat;
}

//
// Clear and deallocate the cache.
//
void cache_destroy(match_ctx_t *ctx)
{
    cache_t *cache = ctx->cache;
    if (cache->hits) delete(&cache->hits);
    cache->occupancy = 0;
    cache->size = 0;
}

//
// Look up a pattern definition by name.
//
__attribute__((nonnull))
pat_t *lookup(match_ctx_t *ctx, const char *name, size_t namelen)
{
    for (pat_t *def = ctx->defs; def; def = def->args.def.next_def) {
        if (namelen == def->args.def.namelen && strncmp(def->args.def.name, name, namelen) == 0)
            return def->args.def.meaning;
    }
    if (ctx->parent_ctx) return lookup(ctx->parent_ctx, name, namelen);
    return NULL;
}

//
// If the given pattern is a reference, look it up and return the referenced
// pattern. This is used for an optimization to avoid repeated lookups.
//
__attribute__((nonnull(1)))
static inline pat_t *deref(match_ctx_t *ctx, pat_t *pat)
{
    if (pat && pat->type == BP_REF) {
        pat_t *def = lookup(ctx, pat->args.ref.name, pat->args.ref.len);
        if (def) return def;
    }
    return pat;
}

//
// Find and return the first and simplest pattern that will definitely have to
// match for the whole pattern to match (if any). Ideally, this would be a
// string literal that can be quickly scanned for.
//
static pat_t *get_prerequisite(match_ctx_t *ctx, pat_t *pat)
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
        case BP_CHAIN: {
            pat_t *f = p->args.multiple.first;
            // If pattern is something like (|"foo"|), then use "foo" as the first thing to scan for
            p = (f->type == BP_WORD_BOUNDARY || f->type == BP_START_OF_LINE) ? p->args.multiple.second : f;
            break;
        }
        case BP_MATCH: case BP_NOT_MATCH:
            p = p->args.multiple.first; break;
        case BP_REPLACE:
            p = p->args.replace.pat; break;
        case BP_REF: {
            pat_t *p2 = deref(ctx, p);
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
__attribute__((nonnull(1,2,3)))
static match_t *_next_match(match_ctx_t *ctx, const char *str, pat_t *pat, pat_t *skip)
{
    if (pat->type == BP_DEFINITIONS || (pat->type == BP_CHAIN && pat->args.multiple.first->type == BP_DEFINITIONS)) {
        match_ctx_t ctx2 = *ctx;
        ctx2.cache = &(cache_t){0};
        ctx2.parent_ctx = ctx;
        ctx2.defs = pat->type == BP_DEFINITIONS ? pat : pat->args.multiple.first;
        pat_t *match_pat = pat->type == BP_DEFINITIONS ? pat->args.def.meaning : pat->args.multiple.second;
        match_t *m = _next_match(&ctx2, str, match_pat, skip);
        cache_destroy(&ctx2);
        return m;
    }

    // Clear the cache so it's not full of old cache values from different parts of the file:
    cache_destroy(ctx);

    pat = deref(ctx, pat); // Avoid repeated lookups
    pat_t *first = get_prerequisite(ctx, pat);

    // Don't bother looping if this can only match at the start/end:
    if (first->type == BP_START_OF_FILE)
        return match(ctx, str, pat);
    else if (first->type == BP_END_OF_FILE)
        return match(ctx, ctx->end, pat);

    // Performance optimization: if the pattern starts with a string literal,
    // we can just rely on the highly optimized memmem() implementation to skip
    // past areas where we know we won't find a match.
    if (!skip && first->type == BP_STRING && first->min_matchlen > 0 && !ctx->ignorecase) {
        char *found = memmem(str, (size_t)(ctx->end - str), first->args.string, first->min_matchlen);
        str = found ? found : ctx->end;
    } else if (!skip && str > ctx->start && (first->type == BP_START_OF_LINE || first->type == BP_END_OF_LINE)) {
        char *found = memchr(str, '\n', (size_t)(ctx->end - str));
        str = found ? (first->type == BP_START_OF_LINE ? found+1 : found) : ctx->end;
    }

    do {
        match_t *m = match(ctx, str, pat);
        if (m) return m;
        match_t *skipped = skip ? match(ctx, str, skip) : NULL;
        if (skipped) {
            str = skipped->end > str ? skipped->end : str + 1;
            recycle_match(&skipped);
        } else str = next_char(str, ctx->end);
    } while (str < ctx->end);
    return NULL;
}

//
// Attempt to match the given pattern against the input string and return a
// match object, or NULL if no match is found.
// The returned value should be free()'d to avoid memory leaking.
//
static match_t *match(match_ctx_t *ctx, const char *str, pat_t *pat)
{
    switch (pat->type) {
    case BP_DEFINITIONS: {
        match_ctx_t ctx2 = *ctx;
        ctx2.cache = &(cache_t){0};
        ctx2.parent_ctx = ctx;
        ctx2.defs = pat;
        match_t *m = match(&ctx2, str, pat->args.def.meaning);
        cache_destroy(&ctx2);
        return m;
    }
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
            return match(ctx, str, pat->args.leftrec.fallback);
        }
    }
    case BP_ANYCHAR: {
        return (str < ctx->end && *str != '\n') ? new_match(pat, str, next_char(str, ctx->end), NULL) : NULL;
    }
    case BP_ID_START: {
        return (str < ctx->end && isidstart(str, ctx->end)) ? new_match(pat, str, next_char(str, ctx->end), NULL) : NULL;
    }
    case BP_ID_CONTINUE: {
        return (str < ctx->end && isidcontinue(str, ctx->end)) ? new_match(pat, str, next_char(str, ctx->end), NULL) : NULL;
    }
    case BP_START_OF_FILE: {
        return (str == ctx->start) ? new_match(pat, str, str, NULL) : NULL;
    }
    case BP_START_OF_LINE: {
        return (str == ctx->start || str[-1] == '\n') ? new_match(pat, str, str, NULL) : NULL;
    }
    case BP_END_OF_FILE: {
        return (str == ctx->end || (str == ctx->end-1 && *str == '\n')) ? new_match(pat, str, str, NULL) : NULL;
    }
    case BP_END_OF_LINE: {
        return (str == ctx->end || *str == '\n') ? new_match(pat, str, str, NULL) : NULL;
    }
    case BP_WORD_BOUNDARY: {
        return (str == ctx->start || isidcontinue(str, ctx->end) != isidcontinue(prev_char(ctx->start, str), ctx->end)) ?
            new_match(pat, str, str, NULL) : NULL;
    }
    case BP_STRING: {
        if (&str[pat->min_matchlen] > ctx->end) return NULL;
        if (pat->min_matchlen > 0 && (ctx->ignorecase ? strncasecmp : strncmp)(str, pat->args.string, pat->min_matchlen) != 0)
            return NULL;
        return new_match(pat, str, str + pat->min_matchlen, NULL);
    }
    case BP_RANGE: {
        if (str >= ctx->end) return NULL;
        if ((unsigned char)*str < pat->args.range.low || (unsigned char)*str > pat->args.range.high)
            return NULL;
        return new_match(pat, str, str+1, NULL);
    }
    case BP_NOT: {
        match_t *m = match(ctx, str, pat->args.pat);
        if (m != NULL) {
            recycle_match(&m);
            return NULL;
        }
        return new_match(pat, str, str, NULL);
    }
    case BP_UPTO: case BP_UPTO_STRICT: {
        match_t *m = new_match(pat, str, str, NULL);
        pat_t *target = deref(ctx, pat->args.multiple.first),
              *skip = deref(ctx, pat->args.multiple.second);
        if (!target && !skip) {
            while (str < ctx->end && *str != '\n') ++str;
            m->end = str;
            return m;
        }

        size_t child_cap = 0, nchildren = 0;
        for (const char *prev = NULL; prev < str; ) {
            prev = str;
            if (target) {
                match_t *p = match(ctx, str, target);
                if (p != NULL) {
                    recycle_match(&p);
                    m->end = str;
                    return m;
                }
            } else if (str == ctx->end) {
                m->end = str;
                return m;
            }
            if (skip) {
                match_t *s = match(ctx, str, skip);
                if (s != NULL) {
                    str = s->end;
                    if (nchildren+2 >= child_cap) {
                        m->children = grow(m->children, child_cap += 5);
                        for (size_t i = nchildren; i < child_cap; i++) m->children[i] = NULL;
                    }
                    m->children[nchildren++] = s;
                    continue;
                }
            }
            // This isn't in the for() structure because there needs to
            // be at least once chance to match the pattern, even if
            // we're at the end of the string already (e.g. "..$").
            if (str < ctx->end && *str != '\n' && pat->type != BP_UPTO_STRICT)
                str = next_char(str, ctx->end);
        }
        recycle_match(&m);
        return NULL;
    }
    case BP_REPEAT: {
        match_t *m = new_match(pat, str, str, NULL);
        size_t reps = 0;
        ssize_t max = pat->args.repetitions.max;
        pat_t *repeating = deref(ctx, pat->args.repetitions.repeat_pat);
        pat_t *sep = deref(ctx, pat->args.repetitions.sep);
        size_t child_cap = 0, nchildren = 0;
        for (reps = 0; max == -1 || reps < (size_t)max; ++reps) {
            const char *start = str;
            // Separator
            match_t *msep = NULL;
            if (sep != NULL && reps > 0) {
                msep = match(ctx, str, sep);
                if (msep == NULL) break;
                str = msep->end;
            }
            match_t *mp = match(ctx, str, repeating);
            if (mp == NULL) {
                str = start;
                if (msep) recycle_match(&msep);
                break;
            }
            if (mp->end == start && reps > 0) {
                // Since no forward progress was made on either `repeating`
                // or `sep` and BP does not have mutable state, it's
                // guaranteed that no progress will be made on the next
                // loop either. We know that this will continue to loop
                // until reps==max, so let's just cut to the chase instead
                // of looping infinitely.
                if (msep) recycle_match(&msep);
                recycle_match(&mp);
                if (pat->args.repetitions.max == -1)
                    reps = ~(size_t)0;
                else
                    reps = (size_t)pat->args.repetitions.max;
                break;
            }
            if (msep) {
                if (nchildren+2 >= child_cap) {
                    m->children = grow(m->children, child_cap += 5);
                    for (size_t i = nchildren; i < child_cap; i++) m->children[i] = NULL;
                }
                m->children[nchildren++] = msep;
            }

            if (nchildren+2 >= child_cap) {
                m->children = grow(m->children, child_cap += 5);
                for (size_t i = nchildren; i < child_cap; i++) m->children[i] = NULL;
            }
            m->children[nchildren++] = mp;
            str = mp->end;
        }

        if (reps < (size_t)pat->args.repetitions.min) {
            recycle_match(&m);
            return NULL;
        }
        m->end = str;
        return m;
    }
    case BP_AFTER: {
        pat_t *back = deref(ctx, pat->args.pat);
        if (!back) return NULL;

        // We only care about the region from the backtrack pos up to the
        // current pos, so mock it out as a file slice.
        // TODO: this breaks ^/^^/$/$$, but that can probably be ignored
        // because you rarely need to check those in a backtrack.
        match_ctx_t slice_ctx = *ctx;
        slice_ctx.cache = &(cache_t){0};
        slice_ctx.start = ctx->start;
        slice_ctx.end = str;
        for (const char *pos = &str[-(long)back->min_matchlen];
             pos >= ctx->start && (back->max_matchlen == -1 || pos >= &str[-(int)back->max_matchlen]);
             pos = prev_char(ctx->start, pos)) {
            cache_destroy(&slice_ctx);
            slice_ctx.start = (char*)pos;
            match_t *m = match(&slice_ctx, pos, back);
            // Match should not go past str (i.e. (<"AB" "B") should match "ABB", but not "AB")
            if (m && m->end != str)
                recycle_match(&m);
            else if (m) {
                cache_destroy(&slice_ctx);
                return new_match(pat, str, str, MATCHES(m));
            }
            if (pos == ctx->start) break;
            // To prevent extreme performance degradation, don't keep
            // walking backwards endlessly over newlines.
            if (back->max_matchlen == -1 && *pos == '\n') break;
        }
        cache_destroy(&slice_ctx);
        return NULL;
    }
    case BP_BEFORE: {
        match_t *after = match(ctx, str, pat->args.pat);
        return after ? new_match(pat, str, str, MATCHES(after)) : NULL;
    }
    case BP_CAPTURE: {
        match_t *p = match(ctx, str, pat->args.pat);
        return p ? new_match(pat, str, p->end, MATCHES(p)) : NULL;
    }
    case BP_OTHERWISE: {
        match_t *m = match(ctx, str, pat->args.multiple.first);
        return m ? m : match(ctx, str, pat->args.multiple.second);
    }
    case BP_CHAIN: {
        if (pat->args.multiple.first->type == BP_DEFINITIONS) {
            match_ctx_t ctx2 = *ctx;
            ctx2.cache = &(cache_t){0};
            ctx2.parent_ctx = ctx;
            ctx2.defs = pat->args.multiple.first;
            match_t *m = match(&ctx2, str, pat->args.multiple.second);
            cache_destroy(&ctx2);
            return m;
        }

        match_t *m1 = match(ctx, str, pat->args.multiple.first);
        if (m1 == NULL) return NULL;

        match_t *m2;
        // Push backrefs and run matching, then cleanup
        if (m1->pat->type == BP_CAPTURE && m1->pat->args.capture.name) {
            // Temporarily add a rule that the backref name matches the
            // exact string of the original match (no replacements)
            pat_t *backref = bp_raw_literal(m1->start, (size_t)(m1->end - m1->start));
            match_ctx_t ctx2 = *ctx;
            ctx2.cache = &(cache_t){0};
            ctx2.parent_ctx = ctx;
            ctx2.defs = &(pat_t){
                .type = BP_DEFINITIONS,
                .start = m1->pat->start, .end = m1->pat->end,
                .args = {
                    .def = {
                        .name = m1->pat->args.capture.name,
                        .namelen = m1->pat->args.capture.namelen,
                        .meaning = backref,
                    }
                },
            };
            m2 = match(&ctx2, m1->end, pat->args.multiple.second);
            if (!m2) // No need to keep the backref in memory if it didn't match
                delete_pat(&backref, false);
            cache_destroy(&ctx2);
        } else {
            m2 = match(ctx, m1->end, pat->args.multiple.second);
        }

        if (m2 == NULL) {
            recycle_match(&m1);
            return NULL;
        }

        return new_match(pat, str, m2->end, MATCHES(m1, m2));
    }
    case BP_MATCH: case BP_NOT_MATCH: {
        match_t *m1 = match(ctx, str, pat->args.multiple.first);
        if (m1 == NULL) return NULL;

        // <p1>~<p2> matches iff the text of <p1> matches <p2>
        // <p1>!~<p2> matches iff the text of <p1> does not match <p2>
        match_ctx_t slice_ctx = *ctx;
        slice_ctx.cache = &(cache_t){0};
        slice_ctx.start = m1->start;
        slice_ctx.end = m1->end;
        match_t *m2 = _next_match(&slice_ctx, slice_ctx.start, pat->args.multiple.second, NULL);
        if ((!m2 && pat->type == BP_MATCH) || (m2 && pat->type == BP_NOT_MATCH)) {
            cache_destroy(&slice_ctx);
            if (m2) recycle_match(&m2);
            recycle_match(&m1);
            return NULL;
        }
        match_t *ret = new_match(pat, m1->start, m1->end, (pat->type == BP_MATCH) ? MATCHES(m1, m2) : MATCHES(m1));
        cache_destroy(&slice_ctx);
        return ret;
    }
    case BP_REPLACE: {
        match_t *p = NULL;
        if (pat->args.replace.pat) {
            p = match(ctx, str, pat->args.replace.pat);
            if (p == NULL) return NULL;
        }
        return new_match(pat, str, p ? p->end : str, MATCHES(p));
    }
    case BP_REF: {
        cache_hit_t hit = cache_get(ctx, str, pat);
        if (hit.start && !hit.end)
            return NULL;

        pat_t *ref = lookup(ctx, pat->args.ref.name, pat->args.ref.len);
        if (ref == NULL)
            errx(EXIT_FAILURE, "Unknown identifier: '%.*s'", (int)pat->args.ref.len, pat->args.ref.name);

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
        match_ctx_t ctx2 = *ctx;
        ctx2.parent_ctx = ctx;
        ctx2.defs = &(pat_t){
            .type = BP_DEFINITIONS,
                .start = pat->start, .end = pat->end,
                .args = {
                    .def = {
                        .name = pat->args.ref.name,
                        .namelen = pat->args.ref.len,
                        .meaning = &rec_op,
                    }
                },
        };

        const char *prev = str;
        match_t *m = match(&ctx2, str, ref);
        if (m == NULL) {
            cache_save(ctx, str, pat, NULL);
            return NULL;
        }

        while (rec_op.args.leftrec.visits > 0) {
            rec_op.args.leftrec.visits = 0;
            recycle_match(&rec_op.args.leftrec.match);
            rec_op.args.leftrec.match = m;
            prev = m->end;
            match_t *m2 = match(&ctx2, str, ref);
            if (m2 == NULL) break;
            if (m2->end <= prev) {
                recycle_match(&m2);
                break;
            }
            m = m2;
        }

        if (rec_op.args.leftrec.match)
            recycle_match(&rec_op.args.leftrec.match);

        // This match wrapper mainly exists for record-keeping purposes.
        // It also helps with visualization of match results.
        // OPTIMIZE: remove this if necessary
        match_t *wrap = new_match(pat, m->start, m->end, MATCHES(m));
        cache_save(ctx, str, pat, wrap);
        return wrap;
    }
    case BP_NODENT: {
        if (*str != '\n') return NULL;
        const char *start = str;

        const char *p = str;
        while (p > ctx->start && p[-1] != '\n') --p;

        // Current indentation:
        char denter = *p;
        int dents = 0;
        if (denter == ' ' || denter == '\t') {
            for (; *p == denter && p < ctx->end; ++p) ++dents;
        }

        // Subsequent indentation:
        while (*str == '\n' || *str == '\n') ++str;
        for (int i = 0; i < dents; i++)
            if (&str[i] >= ctx->end || str[i] != denter) return NULL;

        return new_match(pat, start, &str[dents], NULL);
    }
    default: {
        errx(EXIT_FAILURE, "Unknown pattern type: %u", pat->type);
        return NULL;
    }
    }
}

//
// Return a match object which can be used (may be allocated or recycled).
//
match_t *new_match(pat_t *pat, const char *start, const char *end, match_t *children[])
{
    match_t *m;
    if (unused_matches) {
        m = unused_matches;
        gc_list_remove(m);
        memset(m, 0, sizeof(match_t));
    } else {
        m = new(match_t);
    }
    // Keep track of the object:
    gc_list_prepend(&in_use_matches, m);

    m->pat = pat;
    m->start = start;
    m->end = end;

    if (children) {
        for (int i = 0; children[i]; i++)
            m->_children[i] = children[i];
        m->children = m->_children;
    }
    return m;
}

//
// If the given match is not currently a child member of another match (or
// otherwise reserved) then put it back in the pool of unused match objects.
//
void recycle_match(match_t **at_m)
{
    match_t *m = *at_m;
    if (m->children) {
        for (int i = 0; m->children[i]; i++)
            recycle_match(&m->children[i]);
        if (m->children != m->_children)
            delete(&m->children);
    }

    gc_list_remove(m);
    (void)memset(m, 0, sizeof(match_t));
    gc_list_prepend(&unused_matches, m);
    *at_m = NULL;
}

//
// Force all match objects into the pool of unused match objects.
//
size_t recycle_all_matches(void)
{
    size_t count = 0;
    for (match_t *m; (m = in_use_matches); ++count) {
        gc_list_remove(m);
        if (m->children && m->children != m->_children)
            delete(&m->children);
        gc_list_prepend(&unused_matches, m);
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
    for (match_t *m; (m = unused_matches); ++count) {
        gc_list_remove(m);
        delete(&m);
    }
    return count;
}

//
// Iterate over matches.
// Usage: for (match_t *m = NULL; next_match(&m, ...); ) {...}
//
bool next_match(match_t **m, const char *start, const char *end, pat_t *pat, pat_t *skip, bool ignorecase)
{
    const char *pos;
    if (*m) {
        // Make sure forward progress is occurring, even after zero-width matches:
        pos = ((*m)->end > (*m)->start) ? (*m)->end : (*m)->end+1;
        recycle_match(m);
    } else {
        pos = start;
    }

    if (!pat) return false;

    match_ctx_t ctx = {
        .cache = &(cache_t){0},
        .start = start,
        .end = end,
        .ignorecase = ignorecase,
    };
    *m = (pos <= end) ? _next_match(&ctx, pos, pat, skip) : NULL;
    cache_destroy(&ctx);
    return *m != NULL;
}

//
// Helper function to track state while doing a depth-first search.
//
__attribute__((nonnull))
static match_t *_get_numbered_capture(match_t *m, int *n)
{
    if (*n == 0) return m;
    if (m->pat->type == BP_CAPTURE && m->pat->args.capture.namelen == 0) {
        if (*n == 1) return m;
        --(*n);
    }
    if (m->children) {
        for (int i = 0; m->children[i]; i++) {
            match_t *cap = _get_numbered_capture(m->children[i], n);
            if (cap) return cap;
        }
    }
    return NULL;
}

//
// Get a specific numbered pattern capture.
//
match_t *get_numbered_capture(match_t *m, int n)
{
    return _get_numbered_capture(m, &n);
}

//
// Get a capture with a specific name.
//
match_t *get_named_capture(match_t *m, const char *name, size_t namelen)
{
    if (m->pat->type == BP_CAPTURE && m->pat->args.capture.name
        && m->pat->args.capture.namelen == namelen
        && strncmp(m->pat->args.capture.name, name, m->pat->args.capture.namelen) == 0)
        return m;
    if (m->children) {
        for (int i = 0; m->children[i]; i++) {
            match_t *cap = get_named_capture(m->children[i], name, namelen);
            if (cap) return cap;
        }
    }
    return NULL;
}

static inline void fputc_safe(FILE *out, char c, print_options_t *opts)
{
    (void)fputc(c, out);
    if (c == '\n' && opts && opts->on_nl) {
        opts->on_nl(out);
        if (opts->replace_color) fprintf(out, "%s", opts->replace_color);
    }
}

void fprint_match(FILE *out, const char *file_start, match_t *m, print_options_t *opts)
{
    if (m->pat->type == BP_REPLACE) {
        const char *text = m->pat->args.replace.text;
        const char *end = &text[m->pat->args.replace.len];
        if (opts && opts->replace_color) fprintf(out, "%s", opts->replace_color);

        // TODO: clean up the line numbering code
        for (const char *r = text; r < end; ) {
            // Capture substitution
            if (*r == '@' && r+1 < end && r[1] != '@') {
                const char *next = r+1;
                // Retrieve the capture value:
                match_t *cap = NULL;
                if (isdigit(*next)) {
                    int n = (int)strtol(next, (char**)&next, 10);
                    cap = get_numbered_capture(m->children[0], n);
                } else {
                    const char *name = next, *name_end = after_name(next, end);
                    if (name_end) {
                        cap = get_named_capture(m->children[0], name, (size_t)(name_end - name));
                        next = name_end;
                        if (next < m->end && *next == ';') ++next;
                    }
                }

                if (cap != NULL) {
                    fprint_match(out, file_start, cap, opts);
                    if (opts && opts->replace_color) fprintf(out, "%s", opts->replace_color);
                    r = next;
                    continue;
                }
            }

            if (*r == '\\') {
                ++r;
                if (*r == 'N') { // \N (nodent)
                    ++r;
                    // Mildly hacky: nodents here are based on the *first line*
                    // of the match. If the match spans multiple lines, or if
                    // the replacement text contains newlines, this may get weird.
                    const char *line_start = m->start;
                    while (line_start > file_start && line_start[-1] != '\n') --line_start;
                    fputc_safe(out, '\n', opts);
                    for (const char *p = line_start; p < m->start && (*p == ' ' || *p == '\t'); ++p)
                        fputc(*p, out);
                    continue;
                }
                fputc_safe(out, unescapechar(r, &r, end), opts);
            } else {
                fputc_safe(out, *r, opts);
                ++r;
            }
        }
    } else {
        if (opts && opts->match_color) fprintf(out, "%s", opts->match_color);
        const char *prev = m->start;
        for (int i = 0; m->children && m->children[i]; i++) {
            match_t *child = m->children[i];
            // Skip children from e.g. zero-width matches like >@foo
            if (!(prev <= child->start && child->start <= m->end &&
                  prev <= child->end && child->end <= m->end))
                continue;
            if (child->start > prev) {
                if (opts && opts->fprint_between) opts->fprint_between(out, prev, child->start, opts->match_color);
                else fwrite(prev, sizeof(char), (size_t)(child->start - prev), out);
            }
            fprint_match(out, file_start, child, opts);
            if (opts && opts->match_color) fprintf(out, "%s", opts->match_color);
            prev = child->end;
        }
        if (m->end > prev) {
            if (opts && opts->fprint_between) opts->fprint_between(out, prev, m->end, opts->match_color);
            else fwrite(prev, sizeof(char), (size_t)(m->end - prev), out);
        }
    }
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
