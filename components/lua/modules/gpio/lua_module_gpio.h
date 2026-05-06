/**
 * @file lua_module_gpio.h
 * @brief Lua C module exposing TuyaOpen GPIO to sandboxed scripts.
 *
 * After registration, Lua scripts can use:
 *   gpio.set_direction(pin, "output")
 *   gpio.set_level(pin, 1)
 *   local v = gpio.get_level(pin)
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __LUA_MODULE_GPIO_H__
#define __LUA_MODULE_GPIO_H__

#include "lua_module_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Standard Lua C module open function for "gpio".
 */
int luaopen_gpio(lua_State *L);

/**
 * @brief Register the gpio module with the Lua module registry.
 *
 * Call once during app init (e.g. from tools_register.c).
 */
void lua_module_gpio_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __LUA_MODULE_GPIO_H__ */
