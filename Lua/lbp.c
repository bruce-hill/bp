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
#include "../printmatch.h"
#include "../utils.h"

static const char *builtins_source = (
#include "builtins.h"
);
static int MATCH_METATABLE = 0, PAT_METATABLE = 0;
static pat_t *builtins;

static void push_match(lua_State *L, match_t *m, const char *start);

lua_State *cur_state = NULL;

static void match_error(char **msg)
{
    lua_pushstring(cur_state, *msg);
    free(*msg);
    *msg = NULL;
    lua_error(cur_state);
}

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
    pat_t **pat_storage = (pat_t**)lua_newuserdatauv(L, sizeof(pat_t*), 1);
    *pat_storage = maybe_pat.value.pat;
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1);
    lua_pushlightuserdata(L, (void*)&PAT_METATABLE);
    lua_gettable(L, LUA_REGISTRYINDEX);
    lua_setmetatable(L, -2);
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

static match_t *get_first_capture(match_t *m)
{
    if (m->pat->type == BP_TAGGED) {
        return m;
    } else if (m->pat->type == BP_CAPTURE && !Match(m->pat, BP_CAPTURE)->name) {
        return m;
    } else if (m->children) {
        for (int i = 0; m->children[i]; i++) {
            match_t *cap = get_first_capture(m->children[i]);
            if (cap) return cap;
        }
    }
    return NULL;
}

static void set_capture_fields(lua_State *L, match_t *m, int *n, const char *start)
{
    if (m->pat->type == BP_CAPTURE) {
        match_t *cap = get_first_capture(m->children[0]);
        if (!cap) cap = m->children[0];
        auto capture = Match(m->pat, BP_CAPTURE);
        if (capture->namelen > 0) {
            lua_pushlstring(L, capture->name, capture->namelen);
            push_match(L, cap, start);
            lua_settable(L, -3);
        } else {
            push_match(L, cap, start);
            lua_seti(L, -2, (*n)++);
        }
    } else if (m->pat->type == BP_TAGGED) {
        push_match(L, m, start);
        lua_seti(L, -2, (*n)++);
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

    if (m->pat->type == BP_TAGGED) {
        auto tagged = Match(m->pat, BP_TAGGED);
        lua_pushlstring(L, tagged->name, tagged->namelen);
        lua_setfield(L, -2, "__tag");
    }

    int capture_num = 1;
    for (int i = 0; m->children && m->children[i]; i++)
        set_capture_fields(L, m->children[i], &capture_num, start);

    lua_pushinteger(L, 1 + (int)(m->start - start));
    lua_setfield(L, -2, "start");
    lua_pushinteger(L, 1 + (int)(m->end - start));
    lua_setfield(L, -2, "after");
}

static int Lmatch(lua_State *L)
{
    if (lua_isstring(L, 1)) {
        if (Lcompile(L) != 1)
            return 0;
        lua_replace(L, 1);
    }
    pat_t **at_pat = lua_touserdata(L, 1);
    pat_t *pat = at_pat ? *at_pat : NULL;
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
    cur_state = L;
    bp_errhand_t old = bp_set_error_handler(match_error);
    if (next_match(&m, text+index-1, &text[textlen], pat, builtins, NULL, false)) {
        push_match(L, m, text);
        stop_matching(&m);
        ret = 1;
    }
    bp_set_error_handler(old);
    return ret;
}

static int Lreplace(lua_State *L)
{
    if (lua_isstring(L, 1)) {
        if (Lcompile(L) != 1)
            return 0;
        lua_replace(L, 1);
    }
    pat_t **at_pat = lua_touserdata(L, 1);
    pat_t *pat = at_pat ? *at_pat : NULL;
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
    pat_t *rep_pat = maybe_replacement.value.pat;
    cur_state = L;
    bp_errhand_t old = bp_set_error_handler(match_error);
    for (match_t *m = NULL; next_match(&m, text, &text[textlen], rep_pat, builtins, NULL, false); ) {
        fwrite(prev, sizeof(char), (size_t)(m->start - prev), out);
        fprint_match(out, text, m, NULL);
        prev = m->end;
        ++replacements;
    }
    bp_set_error_handler(old);
    fwrite(prev, sizeof(char), (size_t)(&text[textlen] - prev), out);
    fflush(out);
    lua_pushlstring(L, buf, size);
    lua_pushinteger(L, replacements);
    fclose(out);

    delete_pat(&rep_pat, false);

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

static int Lpat_source(lua_State *L)
{
    lua_getiuservalue(L, 1, 1);
    return 1;
}

static int Lpat_tostring(lua_State *L)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    luaL_addstring(&b, "Pattern [[");
    lua_getiuservalue(L, 1, 1);
    luaL_addvalue(&b);
    luaL_addstring(&b, "]]");
    luaL_pushresult(&b);
    return 1;
}

static int Lpat_gc(lua_State *L)
{
    (void)L;
    pat_t **at_pat = lua_touserdata(L, 1);
    pat_t *pat = *at_pat;
    if (pat) delete_pat(at_pat, true);
    (void)pat;
    return 0;
}

static int Lpat_join(lua_State *L, const char *joiner)
{
    if (!lua_isstring(L, 1)) {
        lua_pushcfunction(L, Lpat_source);
        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);
        lua_replace(L, 1);
    }
    if (!lua_isstring(L, 2)) {
        lua_pushcfunction(L, Lpat_source);
        lua_pushvalue(L, 2);
        lua_call(L, 1, 1);
        lua_replace(L, 2);
    }

    lua_pushcfunction(L, Lcompile);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    luaL_addstring(&b, "(");
    lua_pushvalue(L, 1);
    luaL_addvalue(&b);
    luaL_addstring(&b, ")");
    luaL_addstring(&b, joiner);
    luaL_addstring(&b, "(");
    lua_pushvalue(L, 2);
    luaL_addvalue(&b);
    luaL_addstring(&b, ")");

    luaL_pushresult(&b);
    lua_call(L, 1, 1);

    return 1;
}

static int Lpat_concat(lua_State *L)
{
    return Lpat_join(L, " ");
}

static int Lpat_div(lua_State *L)
{
    return Lpat_join(L, " / ");
}

static const luaL_Reg match_metamethods[] = {
    {"__tostring", Lmatch_tostring},
    {NULL, NULL}
};

static const luaL_Reg pat_methods[] = {
    {"match", Lmatch},
    {"replace", Lreplace},
    {"matches", Lmatches},
    {"getsource", Lpat_source},
    {NULL, NULL}
};

static const luaL_Reg pat_metamethods[] = {
    {"__gc", Lpat_gc},
    {"__concat", Lpat_concat},
    {"__div", Lpat_div},
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

public LUALIB_API int luaopen_bp(lua_State *L)
{
    maybe_pat_t maybe_pat = bp_pattern(builtins_source, builtins_source+strlen(builtins_source));
    if (!maybe_pat.success) {
        raise_parse_error(L, maybe_pat);
        return 0;
    }
    builtins = maybe_pat.value.pat;

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
