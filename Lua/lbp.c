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

static int Ltostring(lua_State *L)
{
    match_t **m = (match_t**)lua_touserdata(L, 1);
    push_matchstring(L, *m);
    return 1;
}

static int Lgc(lua_State *L)
{
    match_t **m = (match_t**)lua_touserdata(L, 1);
    recycle_if_unused(m);
    return 0;
}

static int Llen(lua_State *L)
{
    match_t **m = (match_t**)lua_touserdata(L, 1);
    lua_pushinteger(L, (int)((*m)->end - (*m)->start));
    return 1;
}

static int Lindex(lua_State *L)
{
    match_t **m = (match_t**)lua_touserdata(L, 1);
    int type = lua_type(L, 2);
    match_t *ret = NULL;
    if (type == LUA_TNUMBER) {
        int n = luaL_checkinteger(L, 2);
        if (n == 0) {
            push_matchstring(L, *m);
            return 1;
        } else if (n > 0) {
            ret = get_numbered_capture(*m, n);
        }
    } else if (type == LUA_TSTRING) {
        size_t len;
        const char *name = luaL_checklstring(L, 2, &len);
        ret = get_named_capture(*m, name, len);
    }

    if (ret) {
        match_t **userdata = (match_t**)lua_newuserdatauv(L, sizeof(match_t*), 0);
        *userdata = ret;
        lua_pushlightuserdata(L, (void*)&MATCH_METATABLE);
        lua_gettable(L, LUA_REGISTRYINDEX);
        lua_setmetatable(L, -2);
        return 1;
    }
    return 0;
}

static const luaL_Reg Rinstance_metamethods[] =
{
    {"__len", Llen},
    {"__tostring", Ltostring},
    {"__index", Lindex},
    {"__gc", Lgc},
    {NULL, NULL}
};

// static void push_match(lua_State *L, match_t *m)
// {
//     lua_createtable(L, 0, 1);
//     lua_pushlightuserdata(L, (void*)&MATCH_METATABLE);
//     lua_gettable(L, LUA_REGISTRYINDEX);
//     lua_setmetatable(L, -2);

//     push_matchstring(L, m);
//     lua_rawseti(L, -2, 0);
//     int n = 1;
//     assign_captures(L, m, &n);
// }

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
    if (next_match(&m, builtins, text, &text[textlen], maybe_pat.value.pat, NULL, false)) {

        // lua_createtable(L, 0, 1);

        match_t **userdata = (match_t**)lua_newuserdatauv(L, sizeof(match_t*), 0);
        *userdata = m;
        lua_pushlightuserdata(L, (void*)&MATCH_METATABLE);
        lua_gettable(L, LUA_REGISTRYINDEX);
        lua_setmetatable(L, -2);
        // push_matchstring(L, text_file, m);
        // lua_rawseti(L, -2, 0);
        // int n = 1;
        // assign_captures(L, text_file, m, &n);

        lua_pushinteger(L, (int)(m->start - text) + index);
        lua_pushinteger(L, (int)(m->end - m->start));

        // stop_matching(&m);
        ret = 3;
    }

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
    maybe_pat = bp_replacement(maybe_pat.value.pat, rep_text, rep_text + replen);
    if (!maybe_pat.success) {
        raise_parse_error(L, maybe_pat);
        return 0;
    }

    char *buf = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&buf, &size);
    int replacements = 0;
    const char *prev = text;
    for (match_t *m = NULL; next_match(&m, builtins, text, &text[textlen], maybe_pat.value.pat, NULL, false); ) {
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
