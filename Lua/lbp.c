/*
* lbp.c - bp library for lua
* API:
*   bp.match(str, pat[, start_index]) -> nil or (match_text, start_index, match_len_in_source)
*   bp.replace(str, pat, replacement, start_index) -> str with replacements
*/

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "lua.h"
#include "lauxlib.h"

#include "../pattern.h"
#include "../match.h"

// The C API changed from 5.1 to 5.2, so these shims help the code compile on >=5.2
#if LUA_VERSION_NUM >= 502
#define lua_objlen(L, i) lua_rawlen(L, i)
#define luaL_register(L, _, R) luaL_setfuncs(L, R, 0)
#endif

static const char *builtins_source =
#include "builtins.h"
;

static int MATCH_METATABLE = 0;

static def_t *builtins;

static inline void raise_parse_error(lua_State *L, maybe_pat_t m)
{
    size_t err_len = (size_t)(m.value.error.end - m.value.error.start);
    char *buf = calloc(err_len+1, sizeof(char));
    memcpy(buf, m.value.error.start, err_len);
    luaL_error(L, "%s: \"%s\"", m.value.error.msg, buf);
    free(buf);
}

static void push_matchstring(lua_State *L, match_t *m)
{
    char *buf = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&buf, &size);
    fprint_match(out, m->start, m, NULL);
    fflush(out);
    lua_pushlstring(L, buf, size);
    fclose(out);
}

static void push_match(lua_State *L, match_t *m);

static void set_capture_fields(lua_State *L, match_t *m, int *n)
{
    if (m->pat->type == BP_CAPTURE) {
        if (m->pat->args.capture.namelen > 0) {
            lua_pushlstring(L, m->pat->args.capture.name, m->pat->args.capture.namelen);
            push_match(L, m->children[0]);
            lua_settable(L, -3);
        } else {
            push_match(L, m->children[0]);
            lua_seti(L, -2, *(n++));
        }
    } else if (m->children) {
        for (int i = 0; m->children[i]; i++)
            set_capture_fields(L, m->children[i], n);
    }
}

void push_match(lua_State *L, match_t *m)
{
    lua_createtable(L, 1, 0);
    lua_pushlightuserdata(L, (void*)&MATCH_METATABLE);
    lua_gettable(L, LUA_REGISTRYINDEX);
    lua_setmetatable(L, -2);
    push_matchstring(L, m);
    lua_seti(L, -2, 0);
    int capture_num = 1;
    for (int i = 0; m->children && m->children[i]; i++)
        set_capture_fields(L, m->children[i], &capture_num);
}

static int Ltostring(lua_State *L)
{
    lua_geti(L, 1, 0);
    return 1;
}

static const luaL_Reg Rinstance_metamethods[] =
{
    {"__tostring", Ltostring},
    {NULL, NULL}
};

static void recursive_free_pat(pat_t *pat)
{
    // Do a depth-first traversal, freeing everyting along the way:
    if (!pat) return;
    switch (pat->type) {
    case BP_DEFINITION:
        recursive_free_pat(pat->args.def.def);
        recursive_free_pat(pat->args.def.pat);
        break;
    case BP_REPEAT:
        recursive_free_pat(pat->args.repetitions.sep);
        recursive_free_pat(pat->args.repetitions.repeat_pat);
        break;
    case BP_CHAIN: case BP_UPTO: case BP_UPTO_STRICT:
    case BP_OTHERWISE: case BP_NOT_MATCH: case BP_MATCH:
        recursive_free_pat(pat->args.multiple.first);
        recursive_free_pat(pat->args.multiple.second);
        break;
    case BP_REPLACE:
        recursive_free_pat(pat->args.replace.pat);
        break;
    case BP_CAPTURE:
        recursive_free_pat(pat->args.capture.capture_pat);
        break;
    case BP_NOT: case BP_AFTER: case BP_BEFORE:
        recursive_free_pat(pat->args.pat);
        break;
    case BP_LEFTRECURSION:
        recursive_free_pat(pat->args.leftrec.fallback);
        break;
    default: break;
    }
    free_pat(pat);
}

static int Lmatch(lua_State *L)
{
    size_t textlen, patlen;
    const char *text = luaL_checklstring(L, 1, &textlen);
    const char *pat_text = luaL_checklstring(L, 2, &patlen);
    lua_Integer index = luaL_optinteger(L, 3, 1);
    if (index > (lua_Integer)strlen(text)+1)
        return 0;

    maybe_pat_t maybe_pat = bp_pattern(pat_text, pat_text + patlen);
    if (!maybe_pat.success) {
        raise_parse_error(L, maybe_pat);
        return 0;
    }

    match_t *m = NULL;
    int ret = 0;
    if (next_match(&m, builtins, text+index-1, &text[textlen], maybe_pat.value.pat, NULL, false)) {
        push_match(L, m);
        lua_pushinteger(L, (int)(m->start - text) + 1);
        lua_pushinteger(L, (int)(m->end - m->start));
        stop_matching(&m);
        ret = 3;
    }

    recursive_free_pat(maybe_pat.value.pat);

    return ret;
}

static int Lreplace(lua_State *L)
{
    size_t textlen, patlen, replen;
    const char *text = luaL_checklstring(L, 1, &textlen);
    const char *pat_text = luaL_checklstring(L, 2, &patlen);
    const char *rep_text = luaL_checklstring(L, 3, &replen);
    lua_Integer index = luaL_optinteger(L, 4, 1);
    if (index > (lua_Integer)strlen(text)+1)
        index = (lua_Integer)strlen(text)+1;

    maybe_pat_t maybe_pat = bp_pattern(pat_text, pat_text + patlen);
    if (!maybe_pat.success) {
        raise_parse_error(L, maybe_pat);
        return 0;
    }
    maybe_pat_t maybe_replacement = bp_replacement(maybe_pat.value.pat, rep_text, rep_text + replen);
    if (!maybe_replacement.success) {
        recursive_free_pat(maybe_pat.value.pat);
        raise_parse_error(L, maybe_replacement);
        return 0;
    }

    char *buf = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&buf, &size);
    int replacements = 0;
    const char *prev = text;
    for (match_t *m = NULL; next_match(&m, builtins, text, &text[textlen], maybe_replacement.value.pat, NULL, false); ) {
        fwrite(prev, sizeof(char), (size_t)(m->start - prev), out);
        fprint_match(out, text, m, NULL);
        prev = m->end;
        ++replacements;
    }
    fwrite(prev, sizeof(char), (size_t)(&text[textlen] - prev), out);
    fflush(out);
    lua_pushlstring(L, buf, size);
    lua_pushinteger(L, replacements);
    fclose(out);

    // maybe_pat will get freed by this:
    recursive_free_pat(maybe_replacement.value.pat);

    return 2;
}

LUALIB_API int luaopen_bp(lua_State *L)
{
    maybe_pat_t maybe_pat = bp_pattern(builtins_source, builtins_source+strlen(builtins_source));
    if (!maybe_pat.success) {
        raise_parse_error(L, maybe_pat);
        return 0;
    }
    for (pat_t *p = maybe_pat.value.pat; p && p->type == BP_DEFINITION; p = p->args.def.pat)
        builtins = with_def(builtins, p->args.def.namelen, p->args.def.name, p->args.def.def);

    lua_pushlightuserdata(L, (void*)&MATCH_METATABLE);
    lua_createtable(L, 0, 4);
    luaL_register(L, NULL, Rinstance_metamethods);
    lua_settable(L, LUA_REGISTRYINDEX);

    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, Lmatch);
    lua_setfield(L, -2, "match");
    lua_pushcfunction(L, Lreplace);
    lua_setfield(L, -2, "replace");
    // lua_pushcfunction(L, Leach);
    // lua_setfield(L, -1, "each");
    return 1;
}
