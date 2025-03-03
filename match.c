//
// match.c - Code for the BP virtual machine that performs the matching.
//

#include <ctype.h>
#include <err.h>
#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "match.h"
#include "pattern.h"
#include "utils.h"
#include "utf8.h"

#define MAX_CACHE_SIZE (1<<14)

// Cache entries for results of matching a pattern at a string position
typedef struct cache_entry_s {
    bp_pat_t *pat;
    const char *start;
    // Cache entries use a chained scatter approach modeled after Lua's tables
    struct cache_entry_s *next_probe;
} cache_entry_t;

// Cache uses a hash table to store places where matches will always fail
typedef struct {
    unsigned int size, occupancy, next_free;
    cache_entry_t *fails;
} cache_t;

// Data structure for holding ambient state values during matching
typedef struct match_ctx_s {
    struct match_ctx_s *parent_ctx;
    bp_pat_t *defs;
    cache_t *cache;
    const char *start, *end;
    jmp_buf error_jump;
    bool ignorecase;
} match_ctx_t;

// New match objects are either recycled from unused match objects or allocated
// from the heap. While it is in use, the match object is stored in the
// `in_use_matches` linked list. Once it is no longer needed, it is moved to
// the `unused_matches` linked list so it can be reused without the need for
// additional calls to malloc/free. Thus, it is an invariant that every match
// object is in one of these two lists:
static bp_match_t *unused_matches = NULL;
static bp_match_t *in_use_matches = NULL;

static void default_error_handler(char **msg) {
    errx(EXIT_FAILURE, "%s", *msg);
}

static bp_errhand_t error_handler = default_error_handler;

public bp_errhand_t bp_set_error_handler(bp_errhand_t new_handler)
{
    bp_errhand_t old_handler = error_handler;
    error_handler = new_handler;
    return old_handler;
}

#define MATCHES(...) (bp_match_t*[]){__VA_ARGS__, NULL}

__attribute__((hot, nonnull(1,2,3)))
static bp_match_t *match(match_ctx_t *ctx, const char *str, bp_pat_t *pat);
__attribute__((returns_nonnull))
static bp_match_t *new_match(bp_pat_t *pat, const char *start, const char *end, bp_match_t *children[]);

char *error_message = NULL;

__attribute__((format(printf,2,3)))
static inline void match_error(match_ctx_t *ctx, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    if (error_message) free(error_message);
    vasprintf(&error_message, fmt, args);
    va_end(args);
    longjmp(ctx->error_jump, 1);
}

static bp_match_t *clone_match(bp_match_t *m)
{
    if (!m) return NULL;
    bp_match_t *ret = new_match(m->pat, m->start, m->end, NULL);
    if (m->children) {
        size_t child_cap = 0, nchildren = 0;
        if (!m->children[0] || !m->children[1] || !m->children[2]) {
            child_cap = 3;
            ret->children = ret->_children;
        }
        for (int i = 0; m->children[i]; i++) {
            if (nchildren+1 >= child_cap) {
                ret->children = grow(ret->children, child_cap += 5);
                for (size_t j = nchildren; j < child_cap; j++) ret->children[j] = NULL;
            }
            ret->children[nchildren++] = clone_match(m->children[i]);
        }
    }
    return ret;
}

// Prepend to a doubly linked list
static inline void gc_list_prepend(bp_match_t **head, bp_match_t *m)
{
    if (m->gc.home)
        errx(1, "Node already has a home");
    m->gc.home = head;
    m->gc.next = *head;
    if (*head) (*head)->gc.home = &m->gc.next;
    *head = m;
}

// Remove from a doubly linked list
static inline void gc_list_remove(bp_match_t *m)
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
// Check if we have cached a failure to match a given pattern at the given position.
//
static bool has_cached_failure(match_ctx_t *ctx, const char *str, bp_pat_t *pat)
{
    if (!ctx->cache->fails) return false;
    for (cache_entry_t *fail = &ctx->cache->fails[hash(str, pat->id) & (ctx->cache->size-1)]; fail; fail = fail->next_probe) {
        if (fail->pat == pat && fail->start == str)
            return true;
    }
    return false;
}

//
// Insert into the hash table using a chained scatter table approach.
//
static void _hash_insert(cache_t *cache, const char *str, bp_pat_t *pat)
{
    size_t h = hash(str, pat->id) & (cache->size-1);
    if (cache->fails[h].pat == NULL) { // No collision
        cache->fails[h].pat = pat;
        cache->fails[h].start = str;
        cache->fails[h].next_probe = NULL;
        ++cache->occupancy;
        return;
    }

    if (cache->fails[h].pat == pat && cache->fails[h].start == str)
        return; // Duplicate entry, just leave it be

    // Shuffle the colliding entry along to a free space:
    while (cache->fails[cache->next_free].pat) ++cache->next_free;
    cache_entry_t *free_slot = &cache->fails[cache->next_free];
    *free_slot = cache->fails[h];
    size_t h_orig = hash(free_slot->start, free_slot->pat->id) & (cache->size-1);

    // Put the new entry in its desired slot
    cache->fails[h].pat = pat;
    cache->fails[h].start = str;
    cache->fails[h].next_probe = h_orig == h ? free_slot : NULL;
    ++cache->occupancy;

    if (h_orig != h) { // Maintain the chain that points to the colliding entry
        cache_entry_t *prev = &cache->fails[h_orig]; // Start of the chain
        while (prev->next_probe != &cache->fails[h]) prev = prev->next_probe;
        prev->next_probe = free_slot;
    }
}

//
// Save a match in the cache.
//
static void cache_failure(match_ctx_t *ctx, const char *str, bp_pat_t *pat)
{
    cache_t *cache = ctx->cache;
    // Grow the hash if needed (>99% utilization):
    if (cache->occupancy+1 > (cache->size*99)/100) {
        cache_entry_t *old_fails = cache->fails;
        size_t old_size = cache->size;
        cache->size = old_size == 0 ? 16 : 2*old_size;
        cache->fails = new(cache_entry_t[cache->size]);
        cache->next_free = 0;

        // Rehash:
        for (size_t i = 0; i < old_size; i++) {
            if (old_fails[i].pat)
                _hash_insert(cache, old_fails[i].start, old_fails[i].pat);
        }
        if (old_fails) delete(&old_fails);
    }

    _hash_insert(cache, str, pat);
}

//
// Clear and deallocate the cache.
//
void cache_destroy(match_ctx_t *ctx)
{
    cache_t *cache = ctx->cache;
    if (cache->fails) delete(&cache->fails);
    memset(cache, 0, sizeof(cache_t));
}

//
// Look up a pattern definition by name from a definition pattern.
//
static bp_pat_t *_lookup_def(match_ctx_t *ctx, bp_pat_t *defs, const char *name, size_t namelen)
{
    while (defs != NULL) {
        if (defs->type == BP_CHAIN) {
            auto chain = When(defs, BP_CHAIN);
            bp_pat_t *second = _lookup_def(ctx, chain->second, name, namelen);
            if (second) return second;
            defs = chain->first;
        } else if (defs->type == BP_DEFINITIONS) {
            auto def = When(defs, BP_DEFINITIONS);
            if (namelen == def->namelen && strncmp(def->name, name, namelen) == 0)
                return def->meaning;
            defs = def->next_def;
        } else {
            match_error(ctx, "Invalid pattern type in definitions");
            return NULL;
        }
    }
    return NULL;
}

//
// Look up a pattern definition by name from a context.
//
__attribute__((nonnull(2)))
bp_pat_t *lookup_ctx(match_ctx_t *ctx, const char *name, size_t namelen)
{
    for (; ctx; ctx = ctx->parent_ctx) {
        bp_pat_t *def = _lookup_def(ctx, ctx->defs, name, namelen);
        if (def) return def;
    }
    return NULL;
}

//
// If the given pattern is a reference, look it up and return the referenced
// pattern. This is used for an optimization to avoid repeated lookups.
//
__attribute__((nonnull(1)))
static inline bp_pat_t *deref(match_ctx_t *ctx, bp_pat_t *pat)
{
    if (pat && pat->type == BP_REF) {
        auto ref = When(pat, BP_REF);
        bp_pat_t *def = lookup_ctx(ctx, ref->name, ref->len);
        if (def) return def;
    }
    return pat;
}

//
// Find and return the first and simplest pattern that will definitely have to
// match for the whole pattern to match (if any). Ideally, this would be a
// string literal that can be quickly scanned for.
//
static bp_pat_t *get_prerequisite(match_ctx_t *ctx, bp_pat_t *pat)
{
    int derefs = 0;
    for (bp_pat_t *p = pat; p; ) {
        switch (p->type) {
        case BP_BEFORE:
            p = When(p, BP_BEFORE)->pat; break;
        case BP_REPEAT:
            if (When(p, BP_REPEAT)->min == 0)
                return p;
            p = When(p, BP_REPEAT)->repeat_pat; break;
        case BP_CAPTURE:
            p = When(p, BP_CAPTURE)->pat; break;
        case BP_TAGGED:
            p = When(p, BP_TAGGED)->pat; break;
        case BP_CHAIN: {
            auto chain = When(p, BP_CHAIN);
            // If pattern is something like (|"foo"|), then use "foo" as the first thing to scan for
            p = chain->first->max_matchlen == 0 ? chain->second : chain->first;
            break;
        }
        case BP_MATCH:
            p = When(p, BP_MATCH)->pat; break;
        case BP_NOT_MATCH:
            p = When(p, BP_NOT_MATCH)->pat; break;
        case BP_REPLACE:
            p = When(p, BP_REPLACE)->pat; break;
        case BP_REF: {
            if (++derefs > 10) return p; // In case of left recursion
            bp_pat_t *p2 = deref(ctx, p);
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
static bp_match_t *_next_match(match_ctx_t *ctx, const char *str, bp_pat_t *pat, bp_pat_t *skip)
{
    // Clear the cache so it's not full of old cache values from different parts of the file:
    cache_destroy(ctx);

    bp_pat_t *first = get_prerequisite(ctx, pat);

    // Don't bother looping if this can only match at the start/end:
    if (first->type == BP_START_OF_FILE)
        return match(ctx, str, pat);
    else if (first->type == BP_END_OF_FILE)
        return match(ctx, ctx->end, pat);

    // Performance optimization: if the pattern starts with a string literal,
    // we can just rely on the highly optimized memmem() implementation to skip
    // past areas where we know we won't find a match.
    if (!skip && first->type == BP_STRING && first->min_matchlen > 0) {
        char *found = ctx->ignorecase ?
            strcasestr(str, When(first, BP_STRING)->string)
            : memmem(str, (size_t)(ctx->end - str), When(first, BP_STRING)->string, first->min_matchlen);
        str = found ? found : ctx->end;
    } else if (!skip && str > ctx->start && (first->type == BP_START_OF_LINE || first->type == BP_END_OF_LINE)) {
        char *found = memchr(str, '\n', (size_t)(ctx->end - str));
        str = found ? (first->type == BP_START_OF_LINE ? found+1 : found) : ctx->end;
    }

    do {
        bp_match_t *m = match(ctx, str, pat);
        if (m) return m;
        bp_match_t *skipped = skip ? match(ctx, str, skip) : NULL;
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
static bp_match_t *match(match_ctx_t *ctx, const char *str, bp_pat_t *pat)
{
    switch (pat->type) {
    case BP_DEFINITIONS: {
        match_ctx_t ctx2 = *ctx;
        ctx2.cache = &(cache_t){0};
        ctx2.parent_ctx = ctx;
        ctx2.defs = pat;
        bp_match_t *m = match(&ctx2, str, When(pat, BP_DEFINITIONS)->meaning);
        cache_destroy(&ctx2);
        return m;
    }
    case BP_LEFTRECURSION: {
        // Left recursion occurs when a pattern directly or indirectly
        // invokes itself at the same position in the text. It's handled as
        // a special case, but if a pattern invokes itself at a later
        // point, it can be handled with normal recursion.
        // See: left-recursion.md for more details.
        auto leftrec = When(pat, BP_LEFTRECURSION);
        if (str == leftrec->at) {
            leftrec->visited = true;
            return clone_match(leftrec->match);
        } else {
            return match(leftrec->ctx, str, leftrec->fallback);
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
        if (pat->min_matchlen > 0 && (ctx->ignorecase ? strncasecmp : strncmp)(str, When(pat, BP_STRING)->string, pat->min_matchlen) != 0)
            return NULL;
        return new_match(pat, str, str + pat->min_matchlen, NULL);
    }
    case BP_RANGE: {
        if (str >= ctx->end) return NULL;
        auto range = When(pat, BP_RANGE);
        if ((unsigned char)*str < range->low || (unsigned char)*str > range->high)
            return NULL;
        return new_match(pat, str, str+1, NULL);
    }
    case BP_NOT: {
        bp_match_t *m = match(ctx, str, When(pat, BP_NOT)->pat);
        if (m != NULL) {
            recycle_match(&m);
            return NULL;
        }
        return new_match(pat, str, str, NULL);
    }
    case BP_UPTO: case BP_UPTO_STRICT: {
        bp_match_t *m = new_match(pat, str, str, NULL);
        bp_pat_t *target = deref(ctx, pat->type == BP_UPTO ? When(pat, BP_UPTO)->target : When(pat, BP_UPTO_STRICT)->target),
              *skip = deref(ctx, pat->type == BP_UPTO ? When(pat, BP_UPTO)->skip : When(pat, BP_UPTO_STRICT)->skip);
        if (!target && !skip) {
            while (str < ctx->end && *str != '\n') ++str;
            m->end = str;
            return m;
        }

        size_t child_cap = 0, nchildren = 0;
        for (const char *prev = NULL; prev < str; ) {
            prev = str;
            if (target) {
                bp_match_t *p = match(ctx, str, target);
                if (p != NULL) {
                    recycle_match(&p);
                    m->end = str;
                    return m;
                }
            } else if (str == ctx->end || *str == '\n') {
                m->end = str;
                return m;
            }
            if (skip) {
                bp_match_t *s = match(ctx, str, skip);
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
        bp_match_t *m = new_match(pat, str, str, NULL);
        size_t reps = 0;
        auto repeat = When(pat, BP_REPEAT);
        bp_pat_t *repeating = deref(ctx, repeat->repeat_pat);
        bp_pat_t *sep = deref(ctx, repeat->sep);
        size_t child_cap = 0, nchildren = 0;
        for (reps = 0; repeat->max == -1 || reps < (size_t)repeat->max; ++reps) {
            const char *start = str;
            // Separator
            bp_match_t *msep = NULL;
            if (sep != NULL && reps > 0) {
                msep = match(ctx, str, sep);
                if (msep == NULL) break;
                str = msep->end;
            }
            bp_match_t *mp = match(ctx, str, repeating);
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
                if (repeat->max == -1)
                    reps = ~(size_t)0;
                else
                    reps = (size_t)repeat->max;
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

        if (reps < (size_t)repeat->min) {
            recycle_match(&m);
            return NULL;
        }
        m->end = str;
        return m;
    }
    case BP_AFTER: {
        bp_pat_t *back = deref(ctx, When(pat, BP_AFTER)->pat);
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
            bp_match_t *m = match(&slice_ctx, pos, back);
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
        bp_match_t *after = match(ctx, str, When(pat, BP_BEFORE)->pat);
        return after ? new_match(pat, str, str, MATCHES(after)) : NULL;
    }
    case BP_CAPTURE: case BP_TAGGED: {
        bp_pat_t *to_match = pat->type == BP_CAPTURE ? When(pat, BP_CAPTURE)->pat : When(pat, BP_TAGGED)->pat;
        if (!to_match)
            return new_match(pat, str, str, NULL);
        bp_match_t *p = match(ctx, str, to_match);
        return p ? new_match(pat, str, p->end, MATCHES(p)) : NULL;
    }
    case BP_OTHERWISE: {
        bp_match_t *m = match(ctx, str, When(pat, BP_OTHERWISE)->first);
        return m ? m : match(ctx, str, When(pat, BP_OTHERWISE)->second);
    }
    case BP_CHAIN: {
        auto chain = When(pat, BP_CHAIN);
        if (chain->first->type == BP_DEFINITIONS) {
            match_ctx_t ctx2 = *ctx;
            ctx2.cache = &(cache_t){0};
            ctx2.parent_ctx = ctx;
            ctx2.defs = chain->first;
            bp_match_t *m = match(&ctx2, str, chain->second);
            cache_destroy(&ctx2);
            return m;
        }

        bp_match_t *m1 = match(ctx, str, chain->first);
        if (m1 == NULL) return NULL;

        bp_match_t *m2;
        // Push backrefs and run matching, then cleanup
        if (m1->pat->type == BP_CAPTURE && When(m1->pat, BP_CAPTURE)->name && When(m1->pat, BP_CAPTURE)->backreffable) {
            // Temporarily add a rule that the backref name matches the
            // exact string of the original match (no replacements)
            bp_pat_t *backref;
            if (m1->children && m1->children[0]->pat->type == BP_CURDENT) {
                const char *linestart = m1->start;
                while (linestart > ctx->start && linestart[-1] != '\n') --linestart;

                // Current indentation:
                char denter = *linestart;
                size_t dents = 0;
                if (denter == ' ' || denter == '\t') {
                    while (linestart[dents] == denter && &linestart[dents] < ctx->end)
                        ++dents;
                }
                backref = bp_raw_literal(linestart, dents);
            } else {
                backref = bp_raw_literal(m1->start, (size_t)(m1->end - m1->start));
            }
            match_ctx_t ctx2 = *ctx;
            ctx2.cache = &(cache_t){0};
            ctx2.parent_ctx = ctx;
            ctx2.defs = &(bp_pat_t){
                .type = BP_DEFINITIONS,
                .start = m1->pat->start, .end = m1->pat->end,
                .__tagged.BP_DEFINITIONS = {
                    .name = When(m1->pat, BP_CAPTURE)->name,
                    .namelen = When(m1->pat, BP_CAPTURE)->namelen,
                    .meaning = backref,
                },
            };
            m2 = match(&ctx2, m1->end, chain->second);
            if (!m2) // No need to keep the backref in memory if it didn't match
                delete_pat(&backref, false);
            cache_destroy(&ctx2);
        } else {
            m2 = match(ctx, m1->end, chain->second);
        }

        if (m2 == NULL) {
            recycle_match(&m1);
            return NULL;
        }

        return new_match(pat, str, m2->end, MATCHES(m1, m2));
    }
    case BP_MATCH: case BP_NOT_MATCH: {
        bp_pat_t *target = pat->type == BP_MATCH ? When(pat, BP_MATCH)->pat : When(pat, BP_NOT_MATCH)->pat;
        bp_match_t *m1 = match(ctx, str, target);
        if (m1 == NULL) return NULL;

        // <p1>~<p2> matches iff the text of <p1> matches <p2>
        // <p1>!~<p2> matches iff the text of <p1> does not match <p2>
        match_ctx_t slice_ctx = *ctx;
        slice_ctx.cache = &(cache_t){0};
        slice_ctx.start = m1->start;
        slice_ctx.end = m1->end;
        bp_match_t *ret = NULL, *m2 = NULL;
        if (pat->type == BP_MATCH) {
            m2 = _next_match(&slice_ctx, slice_ctx.start, When(pat, BP_MATCH)->must_match, NULL);
            if (m2) ret = new_match(pat, m1->start, m1->end, MATCHES(m1, m2));
        } else {
            m2 = _next_match(&slice_ctx, slice_ctx.start, When(pat, BP_NOT_MATCH)->must_not_match, NULL);
            if (!m2) ret = new_match(pat, m1->start, m1->end, MATCHES(m1));
        }
        cache_destroy(&slice_ctx);
        if (!ret) {
            if (m2) recycle_match(&m2);
            recycle_match(&m1);
        }
        return ret;
    }
    case BP_REPLACE: {
        bp_match_t *p = NULL;
        auto replace = When(pat, BP_REPLACE);
        if (replace->pat) {
            p = match(ctx, str, replace->pat);
            if (p == NULL) return NULL;
        }
        return new_match(pat, str, p ? p->end : str, MATCHES(p));
    }
    case BP_REF: {
        if (has_cached_failure(ctx, str, pat))
            return NULL;

        auto ref_pat = When(pat, BP_REF);
        bp_pat_t *ref = lookup_ctx(ctx, ref_pat->name, ref_pat->len);
        if (ref == NULL) {
            match_error(ctx, "Unknown pattern: '%.*s'", (int)ref_pat->len, ref_pat->name);
            return NULL;
        }

        if (ref->type == BP_LEFTRECURSION)
            return match(ctx, str, ref);

        bp_pat_t rec_op = {
            .type = BP_LEFTRECURSION,
            .start = ref->start, .end = ref->end,
            .min_matchlen = 0, .max_matchlen = -1,
            .__tagged.BP_LEFTRECURSION = {
                .match = NULL,
                .visited = false,
                .at = str,
                .fallback = pat,
                .ctx = (void*)ctx,
            },
        };
        match_ctx_t ctx2 = *ctx;
        ctx2.parent_ctx = ctx;
        ctx2.defs = &(bp_pat_t){
            .type = BP_DEFINITIONS,
                .start = pat->start, .end = pat->end,
                .__tagged.BP_DEFINITIONS = {
                    .name = ref_pat->name,
                    .namelen = ref_pat->len,
                    .meaning = &rec_op,
                },
        };

        bp_match_t *m = match(&ctx2, str, ref);
        // If left recursion was involved, keep retrying while forward progress can be made:
        if (m && rec_op.__tagged.BP_LEFTRECURSION.visited) {
            while (1) {
                const char *prev = m->end;
                rec_op.__tagged.BP_LEFTRECURSION.match = m;
                ctx2.cache = &(cache_t){0};
                bp_match_t *m2 = match(&ctx2, str, ref);
                cache_destroy(&ctx2);
                if (!m2) break;
                if (m2->end <= prev) {
                    recycle_match(&m2);
                    break;
                }
                recycle_match(&m);
                m = m2;
            }
        }

        if (!m) {
            cache_failure(ctx, str, pat);
            return NULL;
        }

        // This match wrapper mainly exists for record-keeping purposes.
        // It also helps with visualization of match results.
        // OPTIMIZE: remove this if necessary
        return new_match(pat, m->start, m->end, MATCHES(m));
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
    case BP_CURDENT: {
        return new_match(pat, str, str, NULL);
    }
    default: {
        match_error(ctx, "Unknown pattern type: %u", pat->type);
        return NULL;
    }
    }
}

//
// Return a match object which can be used (may be allocated or recycled).
//
bp_match_t *new_match(bp_pat_t *pat, const char *start, const char *end, bp_match_t *children[])
{
    bp_match_t *m;
    if (unused_matches) {
        m = unused_matches;
        gc_list_remove(m);
        memset(m, 0, sizeof(bp_match_t));
    } else {
        m = new(bp_match_t);
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
public void recycle_match(bp_match_t **at_m)
{
    bp_match_t *m = *at_m;
    if (m->children) {
        for (int i = 0; m->children[i]; i++)
            recycle_match(&m->children[i]);
        if (m->children != m->_children)
            delete(&m->children);
    }

    gc_list_remove(m);
    (void)memset(m, 0, sizeof(bp_match_t));
    gc_list_prepend(&unused_matches, m);
    *at_m = NULL;
}

//
// Force all match objects into the pool of unused match objects.
//
public size_t recycle_all_matches(void)
{
    size_t count = 0;
    for (bp_match_t *m; (m = in_use_matches); ++count) {
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
public size_t free_all_matches(void)
{
    size_t count = 0;
    recycle_all_matches();
    for (bp_match_t *m; (m = unused_matches); ++count) {
        gc_list_remove(m);
        delete(&m);
    }
    return count;
}

//
// Iterate over matches.
// Usage: for (bp_match_t *m = NULL; next_match(&m, ...); ) {...}
//
public bool next_match(bp_match_t **m, const char *start, const char *end, bp_pat_t *pat, bp_pat_t *defs, bp_pat_t *skip, bool ignorecase)
{
    const char *pos;
    if (*m) {
        // Make sure forward progress is occurring, even after zero-width matches:
        pos = ((*m)->end > (*m)->start) ? (*m)->end : (*m)->end+1;
        recycle_match(m);
    } else {
        pos = start;
    }

    if (!pat) {
        error_handler = default_error_handler;
        return false;
    }

    match_ctx_t ctx = {
        .cache = &(cache_t){0},
        .start = start,
        .end = end,
        .ignorecase = ignorecase,
        .defs = defs,
    };
    if (setjmp(ctx.error_jump) == 0) {
        *m = (pos <= end) ? _next_match(&ctx, pos, pat, skip) : NULL;
        cache_destroy(&ctx);
    } else {
        recycle_all_matches();
        cache_destroy(&ctx);
        *m = NULL;
        if (error_handler)
            error_handler(&error_message);

        if (error_message) {
            free(error_message);
            error_message = NULL;
        }
    }
    return *m != NULL;
}

//
// Helper function to track state while doing a depth-first search.
//
__attribute__((nonnull))
static bp_match_t *_get_numbered_capture(bp_match_t *m, int *n)
{
    if ((m->pat->type == BP_CAPTURE && When(m->pat, BP_CAPTURE)->namelen == 0) || m->pat->type == BP_TAGGED) {
        if (*n == 1) {
            return m;
        } else {
            --(*n);
            return NULL;
        }
    }

    if (m->pat->type == BP_CAPTURE || m->pat->type == BP_TAGGED)
        return NULL;

    if (m->children) {
        for (int i = 0; m->children[i]; i++) {
            bp_match_t *cap = _get_numbered_capture(m->children[i], n);
            if (cap) return cap;
        }
    }
    return NULL;
}

//
// Get a specific numbered pattern capture.
//
public bp_match_t *get_numbered_capture(bp_match_t *m, int n)
{
    if (n <= 0) return m;
    if (m->pat->type == BP_TAGGED || m->pat->type == BP_CAPTURE) {
        if (n == 1 && m->pat->type == BP_CAPTURE && When(m->pat, BP_CAPTURE)->namelen == 0) return m;
        if (m->children) {
            for (int i = 0; m->children[i]; i++) {
                bp_match_t *cap = _get_numbered_capture(m->children[i], &n);
                if (cap) return cap;
            }
        }
        return NULL;
    } else {
        return _get_numbered_capture(m, &n);
    }
}

//
// Helper function for get_named_capture()
//
bp_match_t *_get_named_capture(bp_match_t *m, const char *name, size_t namelen)
{
    if (m->pat->type == BP_CAPTURE && When(m->pat, BP_CAPTURE)->name
        && When(m->pat, BP_CAPTURE)->namelen == namelen
        && strncmp(When(m->pat, BP_CAPTURE)->name, name, When(m->pat, BP_CAPTURE)->namelen) == 0)
        return m;

    if (m->pat->type == BP_TAGGED || m->pat->type == BP_CAPTURE)
        return NULL;

    if (m->children) {
        for (int i = 0; m->children[i]; i++) {
            bp_match_t *cap = _get_named_capture(m->children[i], name, namelen);
            if (cap) return cap;
        }
    }
    return NULL;
}

//
// Get a capture with a specific name.
//
public bp_match_t *get_named_capture(bp_match_t *m, const char *name, ssize_t _namelen)
{
    size_t namelen = _namelen < 0 ? strlen(name) : (size_t)_namelen;
    if (m->pat->type == BP_TAGGED) {// || (m->pat->type == BP_CAPTURE && m->pat->args.capture.namelen > 0)) {
        if (m->children) {
            for (int i = 0; m->children[i]; i++) {
                bp_match_t *cap = _get_named_capture(m->children[i], name, namelen);
                if (cap) return cap;
            }
        }
        return NULL;
    } else {
        return _get_named_capture(m, name, namelen);
    }
    return NULL;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
