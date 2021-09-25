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
#include "utils.h"
#include "utf8.h"

#define MAX_CACHE_SIZE (1<<14)

// Cache datastructure
typedef struct {
    size_t size, occupancy;
    match_t **matches;
} cache_t;

// Data structure for various ambient state for matching
typedef struct {
    def_t *defs;
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

// Prepend to a doubly linked list
static inline void list_prepend(match_t **head, match_t *m, match_dll_t *node)
{
    if (node->home)
        errx(1, "Node already has a home");
    node->home = head;
    node->next = *head;
    if (*head) {
        match_dll_t *head_node = (match_dll_t*)((char*)(*head) + ((char*)node - (char*)m));
        head_node->home = &node->next;
    }
    *head = m;
}

// Remove from a doubly linked list
static inline void list_remove(match_t *m, match_dll_t *node)
{
    if (!node->home)
        errx(1, "Attempt to remove something that isn't in a list");
    *node->home = node->next;
    if (node->next) {
        match_dll_t *next_node = (match_dll_t*)((char*)(node->next) + ((char*)node - (char*)m));
        next_node->home = node->home;
    }
    node->home = NULL;
    node->next = NULL;
}

//
// Hash a string position/pattern.
//
static inline size_t hash(const char *str, pat_t *pat)
{
    return (size_t)str + 2*pat->id;
}

//
// Check if we have memoized a pattern match at the given position for the
// given definitions. If a result has been memoized, set *result to the
// memoized value and return true, otherwise return false.
//
static bool cache_get(cache_t *cache, def_t *defs, const char *str, pat_t *pat, match_t **result)
{
    if (!cache->matches) return NULL;
    size_t h = hash(str, pat) & (cache->size-1);
    for (match_t *c = cache->matches[h]; c; c = c->cache.next) {
        if (c->pat == pat && c->defs_id == (defs?defs->id:0) && c->start == str) {
            // If c->end == NULL, that means no match occurs here
            *result = c->end == NULL ? NULL : c;
            return true;
        }
    }
    return false;
}

//
// Remove an item from the cache.
//
static void cache_remove(cache_t *cache, match_t *m)
{
    if (!m->cache.home) return;
    *m->cache.home = m->cache.next;
    if (m->cache.next) m->cache.next->cache.home = m->cache.home;
    m->cache.next = NULL;
    m->cache.home = NULL;
    if (--m->refcount == 0) recycle_if_unused(&m);
    --cache->occupancy;
}

//
// Save a match in the cache.
//
static void cache_save(cache_t *cache, def_t *defs, const char *str, pat_t *pat, match_t *m)
{
    // As a convention, a match with {.pat=pat, .start=str, .end==NULL} is used
    // to memoize the fact that `pat` will *not* match at `str`.
    if (m == NULL) m = new_match(defs, pat, str, NULL, NULL);

    if (cache->occupancy+1 > 3*cache->size) {
        if (cache->size == MAX_CACHE_SIZE) {
            size_t h = hash(m->start, m->pat) & (cache->size-1);
            for (int quota = 2; cache->matches[h] && quota > 0; quota--) {
                match_t *last = cache->matches[h];
                while (last->cache.next) last = last->cache.next;
                cache_remove(cache, last);
            }
        } else {
            match_t **old_matches = cache->matches;
            size_t old_size = cache->size;
            cache->size = old_size == 0 ? 16 : 2*old_size;
            cache->matches = new(match_t*[cache->size]);

            // Rehash:
            if (old_matches) {
                for (size_t i = 0; i < old_size; i++) {
                    for (match_t *o; (o = old_matches[i]); ) {
                        *o->cache.home = o->cache.next;
                        if (o->cache.next) o->cache.next->cache.home = o->cache.home;
                        size_t h = hash(o->start, o->pat) & (cache->size-1);
                        o->cache.home = &(cache->matches[h]);
                        o->cache.next = cache->matches[h];
                        if (cache->matches[h]) cache->matches[h]->cache.home = &o->cache.next;
                        cache->matches[h] = o;
                    }
                }
                free(old_matches);
            }
        }
    }

    size_t h = hash(m->start, m->pat) & (cache->size-1);
    m->cache.home = &(cache->matches[h]);
    m->cache.next = cache->matches[h];
    if (cache->matches[h]) cache->matches[h]->cache.home = &m->cache.next;
    cache->matches[h] = m;
    ++m->refcount;
    ++cache->occupancy;
}

//
// Clear and deallocate the cache.
//
void cache_destroy(cache_t *cache)
{
    if (!cache->matches) return;
    for (size_t i = 0; i < cache->size; i++) {
        while (cache->matches[i])
            cache_remove(cache, cache->matches[i]);
    }
    cache->occupancy = 0;
    delete(&cache->matches);
    cache->size = 0;
}

//
// If the given pattern is a reference, look it up and return the referenced
// pattern. This is used for an optimization to avoid repeated lookups.
//
__attribute__((nonnull(1)))
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
__attribute__((nonnull(1,2,3)))
static match_t *_next_match(match_ctx_t *ctx, const char *str, pat_t *pat, pat_t *skip)
{
    // Prune the unnecessary entries from the cache (those not between start/end)
    if (ctx->cache->matches) {
        for (size_t i = 0; i < ctx->cache->size; i++) {
            for (match_t *m = ctx->cache->matches[i], *next = NULL; m; m = next) {
                next = m->cache.next;
                if (m->start < ctx->start || (m->end ? m->end : m->start) > ctx->end)
                    cache_remove(ctx->cache, m);
            }
        }
    }

    pat = deref(ctx->defs, pat);
    pat_t *first = first_pat(ctx->defs, pat);

    // Performance optimization: if the pattern starts with a string literal,
    // we can just rely on the highly optimized memmem() implementation to skip
    // past areas where we know we won't find a match.
    if (!skip && first->type == BP_STRING && first->min_matchlen > 0 && !ctx->ignorecase) {
        char *found = memmem(str, (size_t)(ctx->end - str), first->args.string, first->min_matchlen);
        str = found ? found : ctx->end;
    }

    if (str > ctx->end) return NULL;

    do {
        match_t *m = match(ctx, str, pat);
        if (m) return m;
        if (first->type == BP_START_OF_FILE) return NULL;
        match_t *s;
        if (skip && (s = match(ctx, str, skip))) {
            str = s->end > str ? s->end : str + 1;
            recycle_if_unused(&s);
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
    case BP_DEFINITION: {
        match_ctx_t ctx2 = *ctx;
        ctx2.defs = with_def(ctx->defs, pat->args.def.namelen, pat->args.def.name, pat->args.def.def);
        match_t *m = match(&ctx2, str, pat->args.def.pat ? pat->args.def.pat : pat->args.def.def);
        free_defs(ctx2.defs, ctx->defs);
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
        return (str < ctx->end && *str != '\n') ? new_match(ctx->defs, pat, str, next_char(str, ctx->end), NULL) : NULL;
    }
    case BP_ID_START: {
        return (str < ctx->end && isidstart(str, ctx->end)) ? new_match(ctx->defs, pat, str, next_char(str, ctx->end), NULL) : NULL;
    }
    case BP_ID_CONTINUE: {
        return (str < ctx->end && isidcontinue(str, ctx->end)) ? new_match(ctx->defs, pat, str, next_char(str, ctx->end), NULL) : NULL;
    }
    case BP_START_OF_FILE: {
        return (str == ctx->start) ? new_match(ctx->defs, pat, str, str, NULL) : NULL;
    }
    case BP_START_OF_LINE: {
        return (str == ctx->start || str[-1] == '\n') ? new_match(ctx->defs, pat, str, str, NULL) : NULL;
    }
    case BP_END_OF_FILE: {
        return (str == ctx->end || (str == ctx->end-1 && *str == '\n')) ? new_match(ctx->defs, pat, str, str, NULL) : NULL;
    }
    case BP_END_OF_LINE: {
        return (str == ctx->end || *str == '\n') ? new_match(ctx->defs, pat, str, str, NULL) : NULL;
    }
    case BP_WORD_BOUNDARY: {
        return (str == ctx->start || isidcontinue(str, ctx->end) != isidcontinue(prev_char(ctx->start, str), ctx->end)) ?
            new_match(ctx->defs, pat, str, str, NULL) : NULL;
    }
    case BP_STRING: {
        if (&str[pat->min_matchlen] > ctx->end) return NULL;
        if (pat->min_matchlen > 0 && (ctx->ignorecase ? strncasecmp : strncmp)(str, pat->args.string, pat->min_matchlen) != 0)
            return NULL;
        return new_match(ctx->defs, pat, str, str + pat->min_matchlen, NULL);
    }
    case BP_RANGE: {
        if (str >= ctx->end) return NULL;
        if ((unsigned char)*str < pat->args.range.low || (unsigned char)*str > pat->args.range.high)
            return NULL;
        return new_match(ctx->defs, pat, str, str+1, NULL);
    }
    case BP_NOT: {
        match_t *m = match(ctx, str, pat->args.pat);
        if (m != NULL) {
            recycle_if_unused(&m);
            return NULL;
        }
        return new_match(ctx->defs, pat, str, str, NULL);
    }
    case BP_UPTO: case BP_UPTO_STRICT: {
        match_t *m = new_match(ctx->defs, pat, str, str, NULL);
        pat_t *target = deref(ctx->defs, pat->args.multiple.first),
              *skip = deref(ctx->defs, pat->args.multiple.second);
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
                    recycle_if_unused(&p);
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
                    add_owner(&m->children[nchildren++], s);
                    continue;
                }
            }
            // This isn't in the for() structure because there needs to
            // be at least once chance to match the pattern, even if
            // we're at the end of the string already (e.g. "..$").
            if (str < ctx->end && *str != '\n' && pat->type != BP_UPTO_STRICT)
                str = next_char(str, ctx->end);
        }
        recycle_if_unused(&m);
        return NULL;
    }
    case BP_REPEAT: {
        match_t *m = new_match(ctx->defs, pat, str, str, NULL);
        size_t reps = 0;
        ssize_t max = pat->args.repetitions.max;
        pat_t *repeating = deref(ctx->defs, pat->args.repetitions.repeat_pat);
        pat_t *sep = deref(ctx->defs, pat->args.repetitions.sep);
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
                    m->children = grow(m->children, child_cap += 5);
                    for (size_t i = nchildren; i < child_cap; i++) m->children[i] = NULL;
                }
                add_owner(&m->children[nchildren++], msep);
            }

            if (nchildren+2 >= child_cap) {
                m->children = grow(m->children, child_cap += 5);
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
        pat_t *back = deref(ctx->defs, pat->args.pat);
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
            cache_destroy(slice_ctx.cache);
            slice_ctx.start = (char*)pos;
            match_t *m = match(&slice_ctx, pos, back);
            // Match should not go past str (i.e. (<"AB" "B") should match "ABB", but not "AB")
            if (m && m->end != str)
                recycle_if_unused(&m);
            else if (m) {
                cache_destroy(slice_ctx.cache);
                return new_match(ctx->defs, pat, str, str, MATCHES(m));
            }
            if (pos == ctx->start) break;
            // To prevent extreme performance degradation, don't keep
            // walking backwards endlessly over newlines.
            if (back->max_matchlen == -1 && *pos == '\n') break;
        }
        cache_destroy(slice_ctx.cache);
        return NULL;
    }
    case BP_BEFORE: {
        match_t *after = match(ctx, str, pat->args.pat);
        return after ? new_match(ctx->defs, pat, str, str, MATCHES(after)) : NULL;
    }
    case BP_CAPTURE: {
        match_t *p = match(ctx, str, pat->args.pat);
        return p ? new_match(ctx->defs, pat, str, p->end, MATCHES(p)) : NULL;
    }
    case BP_OTHERWISE: {
        match_t *m = match(ctx, str, pat->args.multiple.first);
        return m ? m : match(ctx, str, pat->args.multiple.second);
    }
    case BP_CHAIN: {
        match_t *m1 = match(ctx, str, pat->args.multiple.first);
        if (m1 == NULL) return NULL;

        match_t *m2;
        // Push backrefs and run matching, then cleanup
        if (m1->pat->type == BP_CAPTURE && m1->pat->args.capture.name) {
            // Temporarily add a rule that the backref name matches the
            // exact string of the original match (no replacements)
            pat_t *backref = bp_raw_literal(m1->start, (size_t)(m1->end - m1->start));
            match_ctx_t ctx2 = *ctx;
            ctx2.defs = with_def(ctx->defs, m1->pat->args.capture.namelen, m1->pat->args.capture.name, backref);
            ++m1->refcount; {
                m2 = match(&ctx2, m1->end, pat->args.multiple.second);
                if (!m2) { // No need to keep the backref in memory if it didn't match
                    free_pat(backref);
                    backref = NULL;
                }
                free_defs(ctx2.defs, ctx->defs);
            } --m1->refcount;
        } else {
            m2 = match(ctx, m1->end, pat->args.multiple.second);
        }

        if (m2 == NULL) {
            recycle_if_unused(&m1);
            return NULL;
        }

        return new_match(ctx->defs, pat, str, m2->end, MATCHES(m1, m2));
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
            cache_destroy(slice_ctx.cache);
            if (m2) recycle_if_unused(&m2);
            recycle_if_unused(&m1);
            return NULL;
        }
        match_t *ret = new_match(ctx->defs, pat, m1->start, m1->end, (pat->type == BP_MATCH) ? MATCHES(m1, m2) : MATCHES(m1));
        cache_destroy(slice_ctx.cache);
        return ret;
    }
    case BP_REPLACE: {
        match_t *p = NULL;
        if (pat->args.replace.pat) {
            p = match(ctx, str, pat->args.replace.pat);
            if (p == NULL) return NULL;
        }
        return new_match(ctx->defs, pat, str, p ? p->end : str, MATCHES(p));
    }
    case BP_REF: {
        match_t *cached;
        if (cache_get(ctx->cache, ctx->defs, str, pat, &cached))
            return cached;

        def_t *def = lookup(ctx->defs, pat->args.ref.len, pat->args.ref.name);
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
        match_ctx_t ctx2 = *ctx;
        ctx2.defs = &(def_t){
            .namelen = def->namelen,
            .name = def->name,
            .pat = &rec_op,
            .next = ctx->defs,
        };

        const char *prev = str;
        match_t *m = match(&ctx2, str, ref);
        if (m == NULL) {
            cache_save(ctx->cache, ctx->defs, str, pat, NULL);
            return NULL;
        }

        while (rec_op.args.leftrec.visits > 0) {
            rec_op.args.leftrec.visits = 0;
            remove_ownership(&rec_op.args.leftrec.match);
            add_owner(&rec_op.args.leftrec.match, m);
            prev = m->end;
            match_t *m2 = match(&ctx2, str, ref);
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
        match_t *wrap = new_match(ctx->defs, pat, m->start, m->end, MATCHES(m));
        cache_save(ctx->cache, ctx->defs, str, pat, wrap);

        if (rec_op.args.leftrec.match)
            remove_ownership(&rec_op.args.leftrec.match);

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

        return new_match(ctx->defs, pat, start, &str[dents], NULL);
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
match_t *new_match(def_t *defs, pat_t *pat, const char *start, const char *end, match_t *children[])
{
    match_t *m;
    if (unused_matches) {
        m = unused_matches;
        list_remove(m, &m->gc);
        memset(m, 0, sizeof(match_t));
    } else {
        m = new(match_t);
    }
    // Keep track of the object:
    list_prepend(&in_use_matches, m, &m->gc);

    m->pat = pat;
    m->start = start;
    m->end = end;
    m->defs_id = (defs?defs->id:0);

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
            delete(&m->children);
    }

    list_remove(m, &m->gc);
    (void)memset(m, 0, sizeof(match_t));
    list_prepend(&unused_matches, m, &m->gc);
    *at_m = NULL;
}

//
// Force all match objects into the pool of unused match objects.
//
size_t recycle_all_matches(void)
{
    size_t count = 0;
    while (in_use_matches) {
        match_t *m = in_use_matches;
        list_remove(m, &m->gc);
        if (m->children && m->children != m->_children)
            delete(&m->children);
        list_prepend(&unused_matches, m, &m->gc);
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
        list_remove(m, &m->gc);
        free(m);
        ++count;
    }
    return count;
}

//
// Iterate over matches.
// Usage: for (match_t *m = NULL; next_match(&m, ...); ) {...}
//
bool next_match(match_t **m, def_t *defs, const char *start, const char *end, pat_t *pat, pat_t *skip, bool ignorecase)
{
    static cache_t cache = {0};
    if (!pat) { // Cleanup for stop_matching()
        recycle_if_unused(m);
        cache_destroy(&cache);
        return false;
    }

    const char *pos;
    if (*m) {
        // Make sure forward progress is occurring, even after zero-width matches:
        pos = ((*m)->end > (*m)->start) ? (*m)->end : (*m)->end+1;
        recycle_if_unused(m);
    } else {
        pos = start;
        cache_destroy(&cache);
    }

    match_ctx_t ctx = {
        .defs = defs,
        .cache = &cache,
        .start = start,
        .end = end,
        .ignorecase = ignorecase,
    };
    *m = (pos <= end) ? _next_match(&ctx, pos, pat, skip) : NULL;
    if (!*m) cache_destroy(&cache);
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
                    const char *name = next, *end = after_name(next);
                    if (end > name) {
                        cap = get_named_capture(m->children[0], name, (size_t)(end - name));
                        next = end;
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
                fputc_safe(out, unescapechar(r, &r), opts);
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
