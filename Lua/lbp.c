/*
* lbp.c - bp library for lua
* API:
*   bp.match(str, pat[, start_index]) -> nil or (match_text, start_index, match_len_in_source)
*   bp.replace(str, pat, replacement, start_index) -> str with replacements
*/

#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "../print.h"
#include "../files.h"
#include "../pattern.h"
#include "../match.h"

// The C API changed from 5.1 to 5.2, so these shims help the code compile on >=5.2
#if LUA_VERSION_NUM >= 502
#define lua_objlen(L, i) lua_rawlen(L, i)
#define luaL_register(L, _, R) luaL_setfuncs(L, R, 0)
#endif

static int MATCH_METATABLE = 0;

static inline void push_parse_error(lua_State *L, maybe_pat_t m)
{
    size_t err_len = (size_t)(m.value.error.end - m.value.error.start);
    char *buf = calloc(err_len+1, sizeof(char));
    memcpy(buf, m.value.error.start, err_len);
    luaL_error(L, "%s: \"%s\"", m.value.error.msg, buf);
    free(buf);
}

static void push_matchstring(lua_State *L, file_t *f, match_t *m)
{
    char *buf = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&buf, &size);
    printer_t pr = {
        .file = f,
        .context_before = NO_CONTEXT,
        .context_after = NO_CONTEXT,
        .use_color = 0,
        .lineformat = "",
    };
    print_match(out, &pr, m);
    fflush(out);
    lua_pushlstring(L, buf, size);
    fclose(out);
}

static int Ltostring(lua_State *L)
{
    match_t **m = (match_t**)lua_touserdata(L, 1);
    push_matchstring(L, NULL, *m);
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
            push_matchstring(L, NULL, *m);
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

    file_t *pat_file = spoof_file(NULL, "<pattern argument>", pat_text, patlen);
    maybe_pat_t maybe_pat = bp_pattern(pat_file->start, pat_file->end);
    if (!maybe_pat.success) {
        push_parse_error(L, maybe_pat);
        destroy_file(&pat_file);
        return 0;
    }

    file_t *text_file = spoof_file(NULL, "<text argument>", text+(index-1), textlen);
    match_t *m = NULL;
    int ret = 0;
    if (next_match(&m, NULL, text_file, maybe_pat.value.pat, NULL, false)) {

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

        lua_pushinteger(L, (int)(m->start - text_file->start) + index);
        lua_pushinteger(L, (int)(m->end - m->start));

        // stop_matching(&m);
        ret = 3;
    }

    // destroy_file(&pat_file);
    // destroy_file(&text_file);
    

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

    file_t *pat_file = spoof_file(NULL, "<pattern argument>", pat_text, patlen);
    maybe_pat_t maybe_pat = bp_pattern(pat_file->start, pat_file->end);
    if (!maybe_pat.success) {
        push_parse_error(L, maybe_pat);
        destroy_file(&pat_file);
        return 0;
    }
    file_t *rep_file = spoof_file(NULL, "<replacement argument>", rep_text, replen);
    maybe_pat = bp_replacement(maybe_pat.value.pat, rep_file->start, rep_file->end);
    if (!maybe_pat.success) {
        push_parse_error(L, maybe_pat);
        destroy_file(&pat_file);
        destroy_file(&rep_file);
        return 0;
    }

    file_t *text_file = spoof_file(NULL, "<text argument>", text+(index-1), textlen);
    char *buf = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&buf, &size);
    printer_t pr = {
        .file = text_file,
        .context_before = ALL_CONTEXT,
        .context_after = ALL_CONTEXT,
        .use_color = 0,
        .lineformat = "",
    };
    int replacements = 0;
    for (match_t *m = NULL; next_match(&m, NULL, text_file, maybe_pat.value.pat, NULL, false); ) {
        print_match(out, &pr, m);
        ++replacements;
    }
    print_match(out, &pr, NULL);
    fflush(out);
    lua_pushlstring(L, buf, size);
    lua_pushinteger(L, replacements);
    fclose(out);

    // destroy_file(&pat_file);
    // destroy_file(&rep_file);
    // destroy_file(&text_file);
    
    return 2;
}

LUALIB_API int luaopen_bp(lua_State *L)
{
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
