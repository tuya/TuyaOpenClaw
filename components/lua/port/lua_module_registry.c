/**
 * @file lua_module_registry.c
 * @brief Lightweight Lua C module registry implementation.
 *
 * A simple static array + linear search.  No heap, no mutex —
 * registration happens once at init time from a single thread.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "lua_module_registry.h"
#include "lauxlib.h"

#include <string.h>

typedef struct {
    const char   *name;
    lua_CFunction open_fn;
} lua_module_entry_t;

static lua_module_entry_t s_registry[LUA_MODULE_REGISTRY_MAX];
static size_t             s_count = 0;

void lua_module_register(const char *name, lua_CFunction open_fn)
{
    size_t i;

    if (!name || !name[0] || !open_fn) {
        return;
    }

    for (i = 0; i < s_count; i++) {
        if (strcmp(s_registry[i].name, name) == 0) {
            return; /* already registered */
        }
    }

    if (s_count >= LUA_MODULE_REGISTRY_MAX) {
        return; /* full — increase LUA_MODULE_REGISTRY_MAX if needed */
    }

    s_registry[s_count].name    = name;
    s_registry[s_count].open_fn = open_fn;
    s_count++;
}

void lua_module_load_all(lua_State *L)
{
    size_t i;

    for (i = 0; i < s_count; i++) {
        luaL_requiref(L, s_registry[i].name, s_registry[i].open_fn, 1);
        lua_pop(L, 1); /* remove copy left on stack */
    }
}

size_t lua_module_registry_count(void)
{
    return s_count;
}
