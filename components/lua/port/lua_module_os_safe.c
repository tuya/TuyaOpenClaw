/**
 * @file lua_module_os_safe.c
 * @brief Sandboxed os library — only time() and date(), no execute/remove/rename.
 *
 * Replaces the upstream loslib.c which exposes dangerous functions.
 * Loaded into the global "os" table by linit_sandbox.c.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#define LUA_LIB

#include "lprefix.h"
#include "lua.h"
#include "lauxlib.h"

#include <time.h>
#include <string.h>

#include "tal_api.h"

/* ---------------------------------------------------------------------------
 * os.time([table]) -> number
 *
 *   os.time()          -> current Unix timestamp (seconds)
 *   os.time{year=2026, month=1, day=1, hour=0, min=0, sec=0} -> timestamp
 * --------------------------------------------------------------------------- */
static int lua_os_time(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        /* No argument: return current time. */
        time_t now = (time_t)tal_time_get_posix();
        lua_pushinteger(L, (lua_Integer)now);
        return 1;
    }

    /* Argument is a table: convert to timestamp via mktime. */
    struct tm t = {0};
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "year");
    t.tm_year = (int)luaL_optinteger(L, -1, 1970) - 1900;
    lua_pop(L, 1);

    lua_getfield(L, 1, "month");
    t.tm_mon = (int)luaL_optinteger(L, -1, 1) - 1;
    lua_pop(L, 1);

    lua_getfield(L, 1, "day");
    t.tm_mday = (int)luaL_optinteger(L, -1, 1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "hour");
    t.tm_hour = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    lua_getfield(L, 1, "min");
    t.tm_min = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    lua_getfield(L, 1, "sec");
    t.tm_sec = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    lua_getfield(L, 1, "isdst");
    t.tm_isdst = lua_toboolean(L, -1) ? 1 : 0;
    lua_pop(L, 1);

    time_t result = mktime(&t);
    if (result == (time_t)(-1)) {
        return luaL_error(L, "time result cannot be represented in this OS");
    }
    lua_pushinteger(L, (lua_Integer)result);
    return 1;
}

/* ---------------------------------------------------------------------------
 * os.date([format [, time]]) -> string or table
 *
 *   os.date()                -> same as os.date("%c")
 *   os.date("%Y-%m-%d %H:%M:%S", os.time()) -> formatted string
 *   os.date("*t" [, time])   -> table {year, month, day, hour, min, sec,
 *                                        wday, yday, isdst}
 * --------------------------------------------------------------------------- */
static int lua_os_date(lua_State *L)
{
    const char *fmt = luaL_optstring(L, 1, "%c");
    time_t t = (time_t)luaL_optinteger(L, 2, (lua_Integer)tal_time_get_posix());

    struct tm stm;
    struct tm *result = localtime_r(&t, &stm);
    if (!result) {
        return luaL_error(L, "date result cannot be represented in this OS");
    }

    if (strcmp(fmt, "*t") == 0) {
        lua_newtable(L);
        lua_pushinteger(L, result->tm_year + 1900);
        lua_setfield(L, -2, "year");
        lua_pushinteger(L, result->tm_mon + 1);
        lua_setfield(L, -2, "month");
        lua_pushinteger(L, result->tm_mday);
        lua_setfield(L, -2, "day");
        lua_pushinteger(L, result->tm_hour);
        lua_setfield(L, -2, "hour");
        lua_pushinteger(L, result->tm_min);
        lua_setfield(L, -2, "min");
        lua_pushinteger(L, result->tm_sec);
        lua_setfield(L, -2, "sec");
        lua_pushinteger(L, result->tm_wday + 1); /* Lua: 1=Sunday */
        lua_setfield(L, -2, "wday");
        lua_pushinteger(L, result->tm_yday + 1); /* Lua: 1-based */
        lua_setfield(L, -2, "yday");
        lua_pushboolean(L, result->tm_isdst);
        lua_setfield(L, -2, "isdst");
        return 1;
    }

    char buf[256];
    size_t len = strftime(buf, sizeof(buf), fmt, result);
    if (len == 0 && fmt[0] != '\0') {
        return luaL_error(L, "invalid date format: '%s'", fmt);
    }
    lua_pushlstring(L, buf, len);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Module entry
 * --------------------------------------------------------------------------- */
int luaopen_os_safe(lua_State *L)
{
    static const luaL_Reg os_funcs[] = {
        {"time", lua_os_time},
        {"date", lua_os_date},
        {NULL,   NULL}
    };

    lua_newtable(L);
    luaL_setfuncs(L, os_funcs, 0);
    return 1;
}
