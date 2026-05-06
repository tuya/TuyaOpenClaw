/**
 * @file lua_module_registry.h
 * @brief Lightweight Lua C module registry for DuckyClaw.
 *
 * Hardware / board-specific Lua modules (gpio, i2c, uart, etc.) register
 * themselves here during init.  When a sandboxed Lua script runs,
 * lua_runtime_run_string() calls lua_module_load_all() to make every
 * registered module available via require() / global namespace.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __LUA_MODULE_REGISTRY_H__
#define __LUA_MODULE_REGISTRY_H__

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of C modules that can be registered. */
#ifndef LUA_MODULE_REGISTRY_MAX
#define LUA_MODULE_REGISTRY_MAX 16
#endif

/**
 * @brief Register a C module so it is loaded into every fresh lua_State.
 *
 * Must be called before the first script execution (typically from
 * tools_register.c during app init).  Duplicate names are ignored.
 *
 * @param[in] name    Module name exposed to Lua (e.g. "gpio").
 * @param[in] open_fn Standard luaopen_xxx entry point.
 */
void lua_module_register(const char *name, lua_CFunction open_fn);

/**
 * @brief Load all registered modules into the given lua_State.
 *
 * Called by lua_runtime_run_string() immediately after luaL_openlibs().
 *
 * @param[in,out] L  Fresh lua_State.
 */
void lua_module_load_all(lua_State *L);

/**
 * @brief Return the number of currently registered modules.
 */
size_t lua_module_registry_count(void);

#ifdef __cplusplus
}
#endif

#endif /* __LUA_MODULE_REGISTRY_H__ */
