/*
 * linit_sandbox.c - sandboxed library loader, replacement for upstream Lua linit.c.
 *
 * Opens only the standard libraries that are safe to expose to LLM-generated
 * code on a connected, voice-controlled hardware agent:
 *   base, coroutine, table, string, math, utf8.
 *
 * Intentionally NOT opened:
 *   io          (filesystem access)
 *   os          (process / shell / time-based escape hatches)
 *   package     (require / loadlib / dynamic linking)
 *   debug       (introspection that can break sandboxing)
 *
 * The corresponding upstream sources (liolib.c, loslib.c, loadlib.c) are
 * not compiled into the build, so the symbols are simply unavailable.
 *
 * This file replaces the upstream linit.c which would call luaL_openlibs
 * with the full library set.
 */

#define LUA_LIB

#include "lprefix.h"
#include <stddef.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

/* Forward: safe os subset (time/date only, no execute/remove/rename). */
int luaopen_os_safe(lua_State *L);

/*
 * Upstream lualib.h defines:
 *   #define luaL_openlibs(L)  luaL_openselectedlibs(L, ~0, 0)
 * We keep the macro and provide luaL_openselectedlibs so that
 * luaL_openlibs(L) expands correctly.  The 'load' bitmask tells
 * us which libraries the caller wants; for our sandbox we only
 * honour the safe subset regardless of the bitmask.
 */

static const luaL_Reg lua_safe_libs[] = {
    {LUA_GNAME,       luaopen_base},
    {LUA_COLIBNAME,   luaopen_coroutine},
    {LUA_TABLIBNAME,  luaopen_table},
    {LUA_STRLIBNAME,  luaopen_string},
    {LUA_MATHLIBNAME, luaopen_math},
    {LUA_UTF8LIBNAME, luaopen_utf8},
    {NULL,            NULL}
};

LUALIB_API void luaL_openselectedlibs(lua_State *L, int load, int preload) {
    (void)load;      /* we always load the same safe subset */
    (void)preload;   /* we never preload libraries */

    const luaL_Reg *lib;
    for (lib = lua_safe_libs; lib->func != NULL; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1); /* remove copy left on the stack */
    }

    /* Safe os subset: time() and date() only.  Loaded as the global
     * "os" table so scripts can write os.time() / os.date(). */
    luaL_requiref(L, "os", luaopen_os_safe, 1);
    lua_pop(L, 1);
}
