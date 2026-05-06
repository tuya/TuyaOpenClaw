/**
 * @file lua_module_gpio.c
 * @brief Lua C module exposing TuyaOpen GPIO to sandboxed scripts.
 *
 * Lua API (available after lua_module_gpio_register()):
 *   gpio.set_direction(pin, "output")   -- configure pin direction/mode
 *   gpio.set_level(pin, 1)              -- drive HIGH or LOW
 *   local v = gpio.get_level(pin)       -- read 0 or 1
 *
 * Modes recognised by set_direction:
 *   "input", "output", "input_output",
 *   "output_od", "input_output_od", "disable"
 *
 * Pin numbers are board-native (T5: P0-P55).  The Lua side receives a
 * plain integer that is cast directly to TUYA_GPIO_NUM_E.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "lua_module_gpio.h"

#include "tkl_gpio.h"
#include "lauxlib.h"

#include <string.h>

/* T5AI board pin range (tool_hw.c uses the same limit). */
#define GPIO_PIN_MAX 56

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */
static bool __pin_valid(int pin)
{
    return pin >= 0 && pin < GPIO_PIN_MAX;
}

static bool __parse_mode(const char *mode_str,
                         TUYA_GPIO_DRCT_E *out_direct,
                         TUYA_GPIO_MODE_E *out_mode)
{
    if (strcmp(mode_str, "input") == 0) {
        *out_direct = TUYA_GPIO_INPUT;
        *out_mode   = TUYA_GPIO_PULLUP;
        return true;
    }
    if (strcmp(mode_str, "output") == 0) {
        *out_direct = TUYA_GPIO_OUTPUT;
        *out_mode   = TUYA_GPIO_PUSH_PULL;
        return true;
    }
    if (strcmp(mode_str, "input_output") == 0) {
        *out_direct = TUYA_GPIO_OUTPUT;
        *out_mode   = TUYA_GPIO_PUSH_PULL;
        return true;
    }
    if (strcmp(mode_str, "output_od") == 0) {
        *out_direct = TUYA_GPIO_OUTPUT;
        *out_mode   = TUYA_GPIO_OPENDRAIN;
        return true;
    }
    if (strcmp(mode_str, "input_output_od") == 0) {
        *out_direct = TUYA_GPIO_OUTPUT;
        *out_mode   = TUYA_GPIO_OPENDRAIN;
        return true;
    }
    if (strcmp(mode_str, "disable") == 0) {
        *out_direct = TUYA_GPIO_INPUT;
        *out_mode   = TUYA_GPIO_FLOATING;
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * gpio.set_direction(pin, mode)
 * --------------------------------------------------------------------------- */
static int lua_gpio_set_direction(lua_State *L)
{
    int                 pin      = (int)luaL_checkinteger(L, 1);
    const char         *mode_str = luaL_checkstring(L, 2);
    TUYA_GPIO_DRCT_E    direct;
    TUYA_GPIO_MODE_E    mode;
    TUYA_GPIO_BASE_CFG_T cfg;

    if (!__pin_valid(pin)) {
        return luaL_error(L, "gpio: pin %d out of range (0-%d)", pin, GPIO_PIN_MAX - 1);
    }
    if (!__parse_mode(mode_str, &direct, &mode)) {
        return luaL_error(L, "gpio: invalid mode '%s'", mode_str);
    }

    cfg.mode   = mode;
    cfg.direct = direct;
    cfg.level  = TUYA_GPIO_LEVEL_LOW;

    if (tkl_gpio_init((TUYA_GPIO_NUM_E)pin, &cfg) != OPRT_OK) {
        return luaL_error(L, "gpio: set_direction failed for P%d", pin);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * gpio.set_level(pin, level)
 * --------------------------------------------------------------------------- */
static int lua_gpio_set_level(lua_State *L)
{
    int                 pin   = (int)luaL_checkinteger(L, 1);
    int                 level = (int)luaL_checkinteger(L, 2);
    TUYA_GPIO_BASE_CFG_T cfg;

    if (!__pin_valid(pin)) {
        return luaL_error(L, "gpio: pin %d out of range (0-%d)", pin, GPIO_PIN_MAX - 1);
    }

    /* TuyaOpen requires init before every write. */
    cfg.mode   = TUYA_GPIO_PUSH_PULL;
    cfg.direct = TUYA_GPIO_OUTPUT;
    cfg.level  = (level != 0) ? TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW;

    if (tkl_gpio_init((TUYA_GPIO_NUM_E)pin, &cfg) != OPRT_OK) {
        return luaL_error(L, "gpio: init failed for P%d", pin);
    }

    if (tkl_gpio_write((TUYA_GPIO_NUM_E)pin, cfg.level) != OPRT_OK) {
        return luaL_error(L, "gpio: write failed for P%d", pin);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * gpio.get_level(pin) -> 0 or 1
 * --------------------------------------------------------------------------- */
static int lua_gpio_get_level(lua_State *L)
{
    int                 pin = (int)luaL_checkinteger(L, 1);
    TUYA_GPIO_BASE_CFG_T cfg;
    TUYA_GPIO_LEVEL_E    lvl = TUYA_GPIO_LEVEL_LOW;

    if (!__pin_valid(pin)) {
        return luaL_error(L, "gpio: pin %d out of range (0-%d)", pin, GPIO_PIN_MAX - 1);
    }

    /* TuyaOpen requires init before every read. */
    cfg.mode   = TUYA_GPIO_PULLUP;
    cfg.direct = TUYA_GPIO_INPUT;
    cfg.level  = TUYA_GPIO_LEVEL_LOW;

    if (tkl_gpio_init((TUYA_GPIO_NUM_E)pin, &cfg) != OPRT_OK) {
        return luaL_error(L, "gpio: init failed for P%d", pin);
    }

    if (tkl_gpio_read((TUYA_GPIO_NUM_E)pin, &lvl) != OPRT_OK) {
        return luaL_error(L, "gpio: read failed for P%d", pin);
    }

    lua_pushinteger(L, (lvl == TUYA_GPIO_LEVEL_HIGH) ? 1 : 0);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Module entry point
 * --------------------------------------------------------------------------- */
int luaopen_gpio(lua_State *L)
{
    lua_newtable(L);

    lua_pushcfunction(L, lua_gpio_set_direction);
    lua_setfield(L, -2, "set_direction");

    lua_pushcfunction(L, lua_gpio_set_level);
    lua_setfield(L, -2, "set_level");

    lua_pushcfunction(L, lua_gpio_get_level);
    lua_setfield(L, -2, "get_level");

    return 1; /* return the module table */
}

/* ---------------------------------------------------------------------------
 * Registration helper
 * --------------------------------------------------------------------------- */
void lua_module_gpio_register(void)
{
    lua_module_register("gpio", luaopen_gpio);
}
