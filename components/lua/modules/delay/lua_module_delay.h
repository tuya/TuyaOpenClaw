/**
 * @file lua_module_delay.h
 * @brief Lua C module exposing blocking delay primitives to sandboxed scripts.
 *
 * After registration, Lua scripts can use:
 *   delay.delay_ms(ms)   -- block the current task for `ms` milliseconds
 *   delay.delay_us(us)   -- busy/blocking sleep for `us` microseconds (<= 1e6)
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __LUA_MODULE_DELAY_H__
#define __LUA_MODULE_DELAY_H__

#include "lua_module_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Standard Lua C module open function for "delay".
 */
int luaopen_delay(lua_State *L);

/**
 * @brief Register the delay module with the Lua module registry.
 *
 * Call once during app init (e.g. from tools_register.c).
 */
void lua_module_delay_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __LUA_MODULE_DELAY_H__ */
