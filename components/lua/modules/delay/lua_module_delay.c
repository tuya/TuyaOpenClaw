/**
 * @file lua_module_delay.c
 * @brief Lua C module exposing TuyaOpen blocking delays to sandboxed scripts.
 *
 * Lua API (available after lua_module_delay_register()):
 *   delay.delay_ms(ms)   -- tal_system_sleep, yields the current task
 *   delay.delay_us(us)   -- tkl_system_sleep_us, blocking; capped at 1_000_000
 *
 * The microsecond cap mirrors esp-claw's lua_module_delay: any delay of
 * one second or longer should go through delay_ms() so the scheduler can
 * still run other tasks.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "lua_module_delay.h"

#include "tal_api.h"
#include "tkl_system.h"
#include "lauxlib.h"

/* Maximum microsecond delay accepted (matches esp-claw behaviour). */
#define LUA_MODULE_DELAY_US_MAX 1000000u

/* ---------------------------------------------------------------------------
 * delay.delay_ms(ms)
 * --------------------------------------------------------------------------- */
static int lua_delay_sleep_ms(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms < 0) {
        ms = 0;
    }
    tal_system_sleep((uint32_t)ms);
    return 0;
}

/* ---------------------------------------------------------------------------
 * delay.delay_us(us)
 * --------------------------------------------------------------------------- */
static int lua_delay_sleep_us(lua_State *L)
{
    lua_Integer us = luaL_checkinteger(L, 1);
    if (us < 0) {
        us = 0;
    }
    if ((uint64_t)us > LUA_MODULE_DELAY_US_MAX) {
        return luaL_error(L,
            "delay: delay_us supports 0..%u only, use delay_ms for longer waits",
            (unsigned)LUA_MODULE_DELAY_US_MAX);
    }
    tkl_system_sleep_us((uint32_t)us);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Module entry point
 * --------------------------------------------------------------------------- */
int luaopen_delay(lua_State *L)
{
    lua_newtable(L);

    lua_pushcfunction(L, lua_delay_sleep_ms);
    lua_setfield(L, -2, "delay_ms");

    lua_pushcfunction(L, lua_delay_sleep_us);
    lua_setfield(L, -2, "delay_us");

    return 1; /* return the module table */
}

/* ---------------------------------------------------------------------------
 * Registration helper
 * --------------------------------------------------------------------------- */
void lua_module_delay_register(void)
{
    lua_module_register("delay", luaopen_delay);
}
