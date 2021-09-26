/*
* lbp.c - bp library for lua
* API:
*   bp.match(pat, str, [start_index]) -> nil or match_table
*   bp.replace(pat, replacement, str, [start_index]) -> str with replacements, num_replacements
*   for match_table in bp.matches(pat, str, [start_index]) do ... end
*   bp.compile(pat) -> pattern object
*       pat:match(str, [start_index])
*       pat:replace(replacement, str, [start_index])
*       for match in pat:matches(str, [start_index]) do ... end
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

static const char *builtins_source = (
#include "builtins.h"
);
static int MATCH_METATABLE = 0, PAT_METATABLE = 0;
static def_t *builtins;

static void push_match(lua_State *L, match_t *m, const char *start);

static inline void raise_parse_error(lua_State *L, maybe_pat_t m)
{
    size_t err_len = (size_t)(m.value.error.end - m.value.error.start);
    char *buf = calloc(err_len+1, sizeof(char));
    memcpy(buf, m.value.error.start, err_len);
    luaL_error(L, "%s: \"%s\"", m.value.error.msg, buf);
    free(buf);
}

static int Lcompile(lua_State *L)
{
    size_t patlen;
    const char *pat_text = luaL_checklstring(L, 1, &patlen);
    maybe_pat_t maybe_pat = bp_pattern(pat_text, pat_text + patlen);
    if (!maybe_pat.success) {
        raise_parse_error(L, maybe_pat);
        return 0;
    }
    lua_createtable(L, 2, 0);
    lua_pushlightuserdata(L, (void*)&PAT_METATABLE);
    lua_gettable(L, LUA_REGISTRYINDEX);
    lua_setmetatable(L, -2);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "source");
    lua_pushlightuserdata(L, maybe_pat.value.pat);
    lua_setfield(L, -2, "pattern");
    return 1;
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

static void set_capture_fields(lua_State *L, match_t *m, int *n, const char *start)
{
    if (m->pat->type == BP_CAPTURE) {
        if (m->pat->args.capture.namelen > 0) {
            lua_pushlstring(L, m->pat->args.capture.name, m->pat->args.capture.namelen);
            push_match(L, m->children[0], start);
            lua_settable(L, -3);
        } else {
            push_match(L, m->children[0], start);
            lua_seti(L, -2, *(n++));
        }
    } else if (m->children) {
        for (int i = 0; m->children[i]; i++)
            set_capture_fields(L, m->children[i], n, start);
    }
}

static void push_match(lua_State *L, match_t *m, const char *start)
{
    lua_createtable(L, 1, 2);
    lua_pushlightuserdata(L, (void*)&MATCH_METATABLE);
    lua_gettable(L, LUA_REGISTRYINDEX);
    lua_setmetatable(L, -2);
    push_matchstring(L, m);
    lua_seti(L, -2, 0);

    int capture_num = 1;
    for (int i = 0; m->children && m->children[i]; i++)
        set_capture_fields(L, m->children[i], &capture_num, start);

    lua_pushinteger(L, 1 + (int)(m->start - start));
    lua_setfield(L, -2, "start");
    lua_pushinteger(L, 1 + (int)(m->end - start));
    lua_setfield(L, -2, "after");
}

static void recursively_free_pat(pat_t *pat)
{
    // Do a depth-first traversal, freeing everyting along the way:
    if (!pat) return;
    switch (pat->type) {
    case BP_DEFINITION:
        recursively_free_pat(pat->args.def.def);
        recursively_free_pat(pat->args.def.pat);
        break;
    case BP_REPEAT:
        recursively_free_pat(pat->args.repetitions.sep);
        recursively_free_pat(pat->args.repetitions.repeat_pat);
        break;
    case BP_CHAIN: case BP_UPTO: case BP_UPTO_STRICT:
    case BP_OTHERWISE: case BP_NOT_MATCH: case BP_MATCH:
        recursively_free_pat(pat->args.multiple.first);
        recursively_free_pat(pat->args.multiple.second);
        break;
    case BP_REPLACE:
        recursively_free_pat(pat->args.replace.pat);
        break;
    case BP_CAPTURE:
        recursively_free_pat(pat->args.capture.capture_pat);
        break;
    case BP_NOT: case BP_AFTER: case BP_BEFORE:
        recursively_free_pat(pat->args.pat);
        break;
    case BP_LEFTRECURSION:
        recursively_free_pat(pat->args.leftrec.fallback);
        break;
    default: break;
    }
    free_pat(pat);
}

static int Lmatch(lua_State *L)
{
    if (lua_isstring(L, 1)) {
        if (Lcompile(L) != 1)
            return 0;
        lua_replace(L, 1);
    }
    lua_getfield(L, 1, "pattern");
    pat_t *pat = lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!pat) luaL_error(L, "Not a valid pattern");

    size_t textlen;
    const char *text = luaL_checklstring(L, 2, &textlen);
    lua_Integer index;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "start");
        lua_getfield(L, 3, "after");
        index = luaL_optinteger(L, -1, 1);
        if (lua_rawequal(L, -1, -2))
            ++index;
    } else {
        index = luaL_optinteger(L, 3, 1);
    }
    if (index > (lua_Integer)strlen(text)+1)
        return 0;

    match_t *m = NULL;
    int ret = 0;
    if (next_match(&m, builtins, text+index-1, &text[textlen], pat, NULL, false)) {
        push_match(L, m, text);
        stop_matching(&m);
        ret = 1;
    }
    return ret;
}

static int Lreplace(lua_State *L)
{
    if (lua_isstring(L, 1)) {
        if (Lcompile(L) != 1)
            return 0;
        lua_replace(L, 1);
    }
    lua_getfield(L, 1, "pattern");
    pat_t *pat = lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!pat) luaL_error(L, "Not a valid pattern");

    size_t replen, textlen;
    const char *rep_text = luaL_checklstring(L, 2, &replen);
    const char *text = luaL_checklstring(L, 3, &textlen);
    lua_Integer index = luaL_optinteger(L, 4, 1);
    if (index > (lua_Integer)strlen(text)+1)
        index = (lua_Integer)strlen(text)+1;

    maybe_pat_t maybe_replacement = bp_replacement(pat, rep_text, rep_text + replen);
    if (!maybe_replacement.success) {
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

    free_pat(maybe_replacement.value.pat);

    return 2;
}

static int iter(lua_State *L)
{
    lua_geti(L, 1, 1);
    lua_geti(L, 1, 2);
    lua_replace(L, 1);
    lua_rotate(L, 1, 1);
    return Lmatch(L);
}

static int Lmatches(lua_State *L)
{
    int nargs = lua_gettop(L);
    lua_pushcfunction(L, iter); // iter
    lua_createtable(L, 2, 0); // state: {pat, str}
    if (lua_isstring(L, 1)) {
        if (Lcompile(L) != 1)
            return 0;
    } else {
        lua_pushvalue(L, 1);
    }
    lua_seti(L, -2, 1);
    lua_pushvalue(L, 2);
    lua_seti(L, -2, 2);
    if (nargs >= 3) // start index
        lua_pushvalue(L, 3);
    else lua_pushnil(L);
    return 3;
}

static int Lmatch_tostring(lua_State *L)
{
    lua_geti(L, 1, 0);
    return 1;
}

static int Lpat_tostring(lua_State *L)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    luaL_addstring(&b, "Pattern [[");
    lua_getfield(L, 1, "source");
    luaL_addvalue(&b);
    luaL_addstring(&b, "]]");
    luaL_pushresult(&b);
    return 1;
}

static int Lpat_gc(lua_State *L)
{
    pat_t *pat = lua_touserdata(L, 1);
    if (pat)
        recursively_free_pat(pat);
    return 0;
}

static const luaL_Reg match_metamethods[] = {
    {"__tostring", Lmatch_tostring},
    {NULL, NULL}
};

static const luaL_Reg pat_methods[] = {
    {"match", Lmatch},
    {"replace", Lreplace},
    {"matches", Lmatches},
    {NULL, NULL}
};

static const luaL_Reg pat_metamethods[] = {
    {"__gc", Lpat_gc},
    {"__tostring", Lpat_tostring},
    {"__index", NULL}, // placeholder for pat_methods
    {NULL, NULL}
};

static const luaL_Reg bp_methods[] = {
    {"match", Lmatch},
    {"replace", Lreplace},
    {"compile", Lcompile},
    {"matches", Lmatches},
    {NULL, NULL}
};

LUALIB_API int luaopen_bp(lua_State *L)
{
    maybe_pat_t maybe_pat = bp_pattern(builtins_source, builtins_source+strlen(builtins_source));
    if (!maybe_pat.success) {
        raise_parse_error(L, maybe_pat);
        return 0;
    }
    for (pat_t *p = maybe_pat.value.pat; p && p->type == BP_DEFINITION; p = p->args.def.pat)
        builtins = with_def(builtins, p->args.def.namelen, p->args.def.name, p->args.def.def);

    lua_pushlightuserdata(L, (void*)&PAT_METATABLE);
    luaL_newlib(L, pat_metamethods);
    luaL_newlib(L, pat_methods);
    lua_setfield(L, -2, "__index");
    lua_settable(L, LUA_REGISTRYINDEX);

    lua_pushlightuserdata(L, (void*)&MATCH_METATABLE);
    luaL_newlib(L, match_metamethods);
    lua_settable(L, LUA_REGISTRYINDEX);

    luaL_newlib(L, bp_methods);
    return 1;
}
