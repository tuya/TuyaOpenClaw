/**
 * @file tool_lua.h
 * @brief MCP tool wrapper exposing the embedded Lua 5.5 sandbox to the LLM.
 *
 * Registers a single tool, lua_run_script, that lets the cloud-side
 * model execute an inline Lua snippet on the device. The tool is gated
 * by CONFIG_ENABLE_LUA_TOOL (which itself depends on CONFIG_ENABLE_LUA);
 * when the gate is off the registration call is a no-op stub so that
 * tools_register.c can call it unconditionally.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __TOOL_LUA_H__
#define __TOOL_LUA_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the lua_run_script MCP tool.
 *
 * Idempotent: safe to call once during MCP server bring-up. The tool
 * is registered with two properties:
 *   - code        (string, required): inline Lua 5.5 source.
 *   - timeout_ms  (int,    optional): wall-clock budget (default 3000).
 *
 * @return OPRT_OK on success or when the feature is disabled at compile
 *         time (in which case no tool is registered).
 */
OPERATE_RET tool_lua_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __TOOL_LUA_H__ */
