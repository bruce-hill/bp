/*
* lbp.c - bp library for lua
* API:
*   bp.match(str, pat[, start_index]) -> nil or (match_text, start_index, match_len_in_source)
*   bp.replace(str, pat, replacement, start_index) -> str with replacements
*/

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

static int Lmatch(lua_State *L)
{
    size_t textlen, patlen;
    const char *text = luaL_checklstring(L, 1, &textlen);
    const char *pat_text = luaL_checklstring(L, 2, &patlen);
    lua_Integer index = luaL_optinteger(L, 3, 1);
    if (index > (lua_Integer)strlen(text)+1)
        return 0;

    file_t *pat_file = spoof_file(NULL, "<pattern argument>", pat_text, patlen);
    pat_t *pat = bp_pattern(pat_file, pat_file->start);
    if (!pat) {
        destroy_file(&pat_file);
        luaL_error(L, "Pattern failed to compile: %s", pat_text);
        return 0;
    }

    file_t *text_file = spoof_file(NULL, "<text argument>", text+(index-1), textlen);
    match_t *m = NULL;
    int ret = 0;
    if (next_match(&m, NULL, text_file, pat, NULL, false)) {
        char *buf = NULL;
        size_t size = 0;
        FILE *out = open_memstream(&buf, &size);
        printer_t pr = {
            .file = text_file,
            .context_before = NO_CONTEXT,
            .context_after = NO_CONTEXT,
            .use_color = 0,
            .lineformat = "",
        };
        print_match(out, &pr, m);
        fflush(out);
        lua_pushlstring(L, buf, size);
        lua_pushinteger(L, (int)(m->start - text_file->start) + index);
        lua_pushinteger(L, (int)(m->end - m->start));
        fclose(out);
        stop_matching(&m);
        ret = 3;
    }

    destroy_file(&pat_file);
    destroy_file(&text_file);
    
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
    pat_t *pat = bp_pattern(pat_file, pat_file->start);
    if (!pat) {
        destroy_file(&pat_file);
        luaL_error(L, "Pattern failed to compile: %s", pat_text);
        return 0;
    }
    file_t *rep_file = spoof_file(NULL, "<replacement argument>", rep_text, replen);
    pat = bp_replacement(rep_file, pat, rep_file->start);
    if (!pat) {
        destroy_file(&pat_file);
        destroy_file(&rep_file);
        luaL_error(L, "Replacement failed to compile: %s", rep_text);
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
    for (match_t *m = NULL; next_match(&m, NULL, text_file, pat, NULL, false); ) {
        print_match(out, &pr, m);
        ++replacements;
    }
    print_match(out, &pr, NULL);
    fflush(out);
    lua_pushlstring(L, buf, size);
    lua_pushinteger(L, replacements);
    fclose(out);

    destroy_file(&pat_file);
    destroy_file(&rep_file);
    destroy_file(&text_file);
    
    return 2;
}

LUALIB_API int luaopen_bp(lua_State *L)
{
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, Lmatch);
    lua_setfield(L, -2, "match");
    lua_pushcfunction(L, Lreplace);
    lua_setfield(L, -2, "replace");
    // lua_pushcfunction(L, Leach);
    // lua_setfield(L, -1, "each");
    return 1;
}
